# RFC: Chamber-heater link — seal the vent while the DragonBreath is heating

Status: **📋 Proposed (RFC) — not started.** Design record for discussion; no code yet.

## Motivation

During a **heat soak** the vent stays open until the print actually starts, which
fights the soak — you're trying to build chamber heat while the vent dumps it. The
vent's AUTO policy can't currently tell "hot because I'm *deliberately* heating up"
from "hot because a print just finished," so a hot bed before a print reads as
residual heat and the vent opens.

The cleanest signal that "we are intentionally building chamber heat, so seal" is a
**chamber heater that is actively heating** — i.e. a **DragonBreath**. When the
DragonBreath is holding a chamber setpoint, the vent should seal, regardless of
print state or detected filament.

## Goal

Add an optional, **printer-source-agnostic** link so DragonVent seals while a paired
**DragonBreath** is actively heating the chamber.

- Works with **any** printer source (Klipper, Bambu LAN, Home Assistant, standalone) —
  the trigger comes from the DragonBreath directly, not the printer.
- No dependency on Klipper, Moonraker, or the dragonbreath-klipper helper.
- Purely additive to the existing AUTO policy; off by default; safe when the
  DragonBreath is unreachable.

## Non-goals

- Bidirectional control (the DragonBreath reading the vent) — out of scope; this RFC
  is vent-reads-breath only.
- Commanding the DragonBreath from the vent — read-only status, like the Bambu source.
- Replacing the existing material/bed AUTO logic — this is an added, higher-precedence
  seal trigger, not a rewrite.

## Why a direct link (not via Moonraker)

An alternative is for the vent to read the DragonBreath's Klipper object
(`heater_generic dragonbreath`) off the shared Moonraker it may already be subscribed
to. Rejected as the primary design because it only works when the vent's source is
Klipper **and** the dragonbreath-klipper helper is installed **and** the heater name
matches — excluding Bambu/HA/standalone users who may run a Panda Breath + Panda Vent
together. A direct link is source-agnostic and keeps the coupling inside the Dragon
family.

## Design

### Discovery & configuration
A new optional setup section, **"Chamber heater (DragonBreath)"**:
- **Enable** toggle (default off).
- **Host** — the DragonBreath address (e.g. `dragonbreath.local` or its IP). Offer a
  **scan** button that mDNS-discovers DragonBreath hosts on the LAN and fills the
  field, mirroring the Bambu-discovery UX already in the family SPA.
- **Control token** — only needed if the DragonBreath has a control token set
  (sent as `X-Dragon-Auth`, same header the family already uses).

Persisted to NVS alongside the other vent config; rendered by the shared schema-driven
setup page (no new firmware UI page).

### Transport
Poll the DragonBreath's existing read-only state API:
- `GET http://<host>/api/v2/state` every **~3–5 s** (matches the vent's existing
  cadence; the DragonBreath already serves this to its own dashboard).
- Auth header `X-Dragon-Auth: <token>` (or the presence-only `web` sentinel when no
  token is set), exactly as the family SPA does.
- This introduces an `esp_http_client` poller to the vent (a new transport here — the
  vent currently speaks websocket/MQTT only). A small dedicated task, or a reuse if a
  shared client lands. *(Alternative: subscribe to the DragonBreath's SSE
  `/api/v2/events` for push instead of polling — lower latency, but an SSE client is
  also new. Polling is simpler for v1.)*

### The "actively heating" signal — mode-aware, not just target > 0
The DragonBreath heats in several modes that mean **different things**, so the trigger
gates on its `mode` (`state.mode`), not merely a nonzero target:

| DragonBreath `mode` | Heating? | Vent |
|---|---|---|
| `off` | no | no seal (normal policy) |
| `filter` (fan-only) | no | no seal — filtering isn't building heat |
| `power_on` (manual) | yes | **seal** — the manual heat-soak case (primary) |
| `auto` | yes *iff* filament has a zone | **seal** when `effective_c > 0` (ABS/ASA/PC); PLA zone = 0 → no seal |
| `drying` | yes | **opt-in** (see below) — default no seal |

So the trigger is:
```
chamber_heating = reachable && not faulted/inhibited
                  && mode ∈ {power_on, auto}          // (+ drying iff opted in)
                  && target.effective_c > 0
```
- **`target.effective_c > 0`** is the **steady** signal (intent to heat/hold). Inside
  `auto` it self-resolves: a seal-material holds a nonzero zone target → seal; PLA's
  zone is 0 → `effective_c = 0` → no seal (matches the vent's own "PLA vents" rule).
- `heater.demand` / `heater.output` **flicker** with the bang-bang control, so they're a
  poor trigger — use the setpoint, not the element drive.

**Dry mode is deliberately opt-in.** Drying is usually standalone (no print), heating
to ~45–55 °C. Sealing *holds* that temp, but drying also *expels moisture*, which argues
for airflow — a genuine preference, and not tied to a print. Gate it behind an explicit
sub-toggle ("also seal while the chamber heater is drying"), **default off**, so the
soak/print path is clean out of the box.

Staleness handling is below.

### Failure / staleness handling
- If the DragonBreath is unreachable, returns an error, or its last good sample is
  older than a timeout (e.g. **2–3 poll intervals**), treat `chamber_heating = false`
  and fall back to the normal AUTO policy — never seal on stale data.
- Link disabled → the feature is completely inert; policy is unchanged.

### AUTO policy integration
The feature lives **only inside the vent's AUTO mode**. If the vent is in MANUAL
(user pinned open/closed), that explicit choice always wins and the breath link is
inert. Within vent-AUTO, `chamber_heating` is a new input with high precedence:

```
vent MANUAL → user's pinned target (breath link inert)
vent AUTO:
  1. unreliable / error            -> hold (unchanged)
  2. chamber_heating (DragonBreath) -> CLOSED   ← NEW overlay
  3. active print + material rule   -> seal / open (unchanged)
  4. idle + bed hysteresis          -> open / close / hold (unchanged)
```

- Seal while the DragonBreath holds a setpoint; when it drops the setpoint (cooldown),
  revert to the normal policy so the existing bed hysteresis vents residual heat.
- Use the steady `effective_c > 0` (not the flickering element drive) so the vent
  doesn't flap; add a short close-hold if needed.

### Handoff & lifecycle
The breath-link seal is a high-precedence **overlay**, not a separate mode — the
"handoff" is just precedence. Walk a full ABS job:

1. **Pre-print soak** — you power on the breath (`power_on`). The overlay seals the vent.
   *(This is the window that stays open today.)*
2. **Print starts** — the breath flips to `auto` @ its zone target, **and** the vent's
   own material rule (ABS → seal) also says CLOSED. They **agree**, so the transition is
   invisible — no flap, no gap. Control passes seamlessly from overlay to base policy.
3. **Print ends** — the breath target → 0 (cooldown), the overlay releases, and the vent
   falls back to idle bed-hysteresis: a still-hot bed → **opens** to vent residual heat
   until cool. This is the one visible transition, and it's the desired behavior.

Because the breath's `auto` seal and the vent's material rule point the same way for
seal-materials, the print-phase transition is seamless; the only handback is at
cooldown. Edge case (open question 4): a manual soak (`power_on`) followed by a
**PLA** print pits the overlay (seal, heater still on) against the material rule (open) —
proposed resolution: while the breath is commanded to heat, sealing wins; turn the breath
off to vent.

### Where the code lives
Two options for discussion:
- **Vent-local** — a `dv_breath` poller + config in the DragonVent repo. Smallest
  blast radius; fastest to ship.
- **Shared core** — a `dc_breath` (or `dc_chamber`) status client in `dragon-core`,
  reusable by any family product, consumed by the vent as the first user. More reusable
  (mirrors how `dc_bambu`/`dc_moonraker` are shared read-only clients), slightly more
  process. **Leaning shared**, since "DragonBreath as a status source" is a family-level
  capability.

## Rough effort
- New status client (config + HTTP poll + parse `target.effective_c` + staleness):
  ~200–300 lines.
- One `dv_policy` input + precedence branch: small.
- Setup-section fields + optional mDNS scan: reuse existing schema + discovery patterns.
- Hardware validation: a paired DragonBreath + DragonVent on the same LAN.

## Open questions
1. **Trigger definition** — proposed `mode ∈ {power_on, auto} && effective_c > 0 &&
   not faulted/inhibited` (see the mode table). Agree on the mode gate vs a plain
   `effective_c > 0`?
2. **Dry mode** — opt-in sub-toggle (default off) as proposed, or seal during drying by
   default, or never? (Moisture-expulsion vs heat-retention tradeoff, and drying is
   usually printless.)
3. **Discovery** — manual host + mDNS scan for v1, or require manual entry initially?
4. **Code home** — `dc_breath` in dragon-core (reusable) vs `dv_breath` in the vent
   (fast)?
5. **Precedence vs an active PLA print** — manual soak then a PLA print: overlay (seal,
   heater on) vs material rule (open). Proposed: sealing wins while the breath is
   commanded to heat.
6. **SSE vs polling** — start with polling; revisit SSE if latency matters.

## Out of scope / future
- Bidirectional (DragonBreath reacting to vent state).
- Auto-pairing multiple DragonBreath units.
- A shared family presence/discovery bus.
