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

### The "actively heating" signal
From the DragonBreath `/api/v2/state`:
- **`target.effective_c > 0`** — the chamber is commanded to a nonzero setpoint. This
  is the **steady** signal and the recommended trigger (intent to heat/hold).
- `heater.demand` / `heater.output` — the element drive; these **flicker** with the
  bang-bang control, so they're a poor direct trigger. Use `target.effective_c` and,
  if desired, treat the heater as "engaged" while a nonzero target is held.

Define `chamber_heating = (effective_c > 0)`, with staleness handling below.

### Failure / staleness handling
- If the DragonBreath is unreachable, returns an error, or its last good sample is
  older than a timeout (e.g. **2–3 poll intervals**), treat `chamber_heating = false`
  and fall back to the normal AUTO policy — never seal on stale data.
- Link disabled → the feature is completely inert; policy is unchanged.

### AUTO policy integration
Add `chamber_heating` as a new input to `dv_policy` with high precedence:

```
1. unreliable / error            -> hold (unchanged)
2. chamber_heating (DragonBreath) -> CLOSED   ← NEW
3. active print + material rule   -> seal / open (unchanged)
4. idle + bed hysteresis          -> open / close / hold (unchanged)
```

- Seal while the DragonBreath holds a setpoint; when it drops the setpoint (cooldown),
  revert to the normal policy so the existing bed hysteresis vents residual heat.
- Use the steady `effective_c > 0` (not the flickering element drive) so the vent
  doesn't flap; add a short close-hold if needed.

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
1. **Trigger definition** — `target.effective_c > 0` alone, or also require the device
   not be OFF/faulted? (Probably: heating ⇔ nonzero effective target and not
   inhibited/faulted.)
2. **Discovery** — manual host + mDNS scan for v1, or require manual entry initially?
3. **Code home** — `dc_breath` in dragon-core (reusable) vs `dv_breath` in the vent
   (fast)? 
4. **Precedence vs an active PLA print** — if the chamber heater is on but a
   vent-preferred material is loaded, does chamber-heating still win? (Proposed: yes —
   an on chamber heater is an explicit heat-retention intent.)
5. **SSE vs polling** — start with polling; revisit SSE if latency matters.

## Out of scope / future
- Bidirectional (DragonBreath reacting to vent state).
- Auto-pairing multiple DragonBreath units.
- A shared family presence/discovery bus.
