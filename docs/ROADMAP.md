# DragonVent Roadmap

Custom firmware for the Bigtreetech Panda Vent hardware that replaces the
stock Bambu Lab MQTT integration with Moonraker/Klipper support.

## Goals

- Repurpose stock Panda Vent hardware for Klipper-based printers
- Automatically open/close the vent based on printer status, material, and
  chamber conditions — matching (and eventually exceeding) stock behavior
- Provide a captive portal AP for WiFi setup (like stock firmware)
- Simple web UI for Moonraker connection, thresholds, and material rules
- OTA firmware updates via web UI
- Complete feature parity with the stock Bigtreetech firmware where it
  makes sense; skip cloud-only features

## Phase 1 — MVP (Hardware Bring-up & Core Logic) — ✅ shipped in v0.2.6

Baseline "vents move on real hardware, don't crash, portal works" release.
Verified against tester OldGuyMeltsPlastic's retail 2-vent kit on
2026-07-10 — 10× open/close cycles with no ESP crash.

### Vent Control via Moonraker
- [x] Connect to Moonraker via shared `dragon-core` WebSocket client (`dc_moonraker`)
- [x] Subscribe to `print_stats` and `heater_bed` objects
- [x] Open vent when bed is heated / printing is active (`dv_policy`, AUTO mode)
- [x] Close vent when bed cools down / printer is idle (35 °C / 45 °C hysteresis)
- [x] Auto/manual mode toggle via physical button (GPIO 12)
- [x] Long-press button to switch between auto and manual mode

### WiFi & Captive Portal
- [x] AP mode on first boot with DNS-redirect captive portal
- [x] Web page to enter SSID + password
- [x] Credentials stored in NVS
- [x] Auto-reconnect on boot
- [x] mDNS `DragonVent.local`

### Motor + Hall
- [x] LEDC PWM forward/reverse with fade-based soft-start
- [x] Hall-sensor position feedback with arrival debounce (30 ms) and
      finite retry (`gave_up` flag) — v0.2.6
- [x] Widened CLOSED band to survive the non-monotonic mid-travel hump

### Portal Parity (Web UI)
- [x] Unified Dragon-family SPA with integrated schema-driven device setup
- [x] Persistent status card (fw version, WiFi, Moonraker, printer state, bed, target, mode)
- [x] Quick Open / Close buttons
- [x] Dark mode via `prefers-color-scheme`
- [x] WiFi scan + hidden-SSID entry, configurable AP hotspot, AP toggle
- [x] OTA `.bin` upload → inactive partition
- [x] Factory reset

### Known limitations we're carrying into 0.3.x

- **RGB status LEDs** are not driven yet. Deferred beyond 0.3.x — see
  [Phase 4](#phase-4--rgb-lighting-deferred). When it lands, WS2812 /
  RMT init must sequence after `dv_motor_init` to keep the stock ADC
  ordering invariant intact.
- ~~Hall thresholds hard-coded in raw counts~~ — fixed in v0.3.3, now
  uses stock's calibrated mV thresholds. See
  [Phase 3](#phase-3--hall-sensor-calibration-parity--shipped-in-v033).
- ~~CLOSED arrives on the mid-travel bump~~ — fixed in v0.3.3 as a
  side effect of the calibrated bands.

## Phase 2 — Deeper Moonraker Integration & Auto Logic (v0.3.0)

Right now the "auto" policy is `printing OR bed>30 → OPEN`. That's enough
to prove it works but ignores everything else Klipper knows. Goal for
0.3.0: read Moonraker as richly as stock reads Bambu MQTT, and use that
data to make smarter open/close decisions — including material-aware
behavior (the tester's core ask).

### Moonraker ingest expansion (`dc_moonraker`)
- [ ] Expand the initial subscribe: add `virtual_sdcard`, `webhooks`,
      `display_status`, `extruder`, and optional `heater_generic chamber`
      alongside the existing `print_stats` + `heater_bed`
- [x] Consume the shared `dc_printer_state_t` enum instead of a bare `printing`
      bool — one of `IDLE`, `PREPARING`, `PRINTING`, `PAUSED`, `COMPLETE`,
      `ERROR` (mirrors the six-state model stock uses on the Bambu side)
- [ ] Track `webhooks.state` so Klipper shutdown / firmware-restart shows
      as `ERROR` rather than "still printing"

### Material awareness
- [ ] Read Klipper `save_variables` (or a well-known gcode-macro variable
      set from `PRINT_START`) to pick up the current material. The tester
      already passes `MATERIAL=` into `PRINT_START` from his slicer, so
      the ingest side is free
- [ ] Configurable per-material rules in the portal: PLA → open above
      45 °C bed, ABS/ASA → stay closed for heat retention, PETG → open
      above 60 °C, etc. Ship sane defaults, let the user edit
- [ ] Bed-temperature thresholds move from hard-coded to per-material
      NVS-backed values (drop the current 35 / 45 °C constants)
- [ ] Fallback rule when material is unknown (probably: current
      bed-temperature hysteresis — what we do today)

### Smarter auto policy (`dv_policy`)
- [x] Consume `dc_printer_state_t` — `COMPLETE` keeps the vent open while
      the bed remains above the close threshold, and `ERROR` holds the current
      state
- [ ] Chamber-temp override when `heater_generic chamber` is present
      (Voron-style enclosures)
- [ ] Manual-mode target survives reboot (currently reset on power cycle)

### Portal surfacing
- [ ] Home tab shows printer state (with the six-state label), material,
      progress %, chamber temp when available
- [ ] Printer tab: material-rule editor, threshold sliders
- [ ] Log/diagnostic tab that mirrors what `dragonvent-diag` sees — the
      hall raw + state stream, motor drive events, Moonraker connection
      health

### Verification before tagging 0.3.0
- [ ] All of the above tested on local devkit for logic correctness
- [ ] Full print cycle run on tester's real hardware — PLA and ABS at
      minimum, ideally with a paused / resumed print thrown in
- [ ] `dragonvent-diag` capture across a whole print, shared for review

## Phase 3 — Hall Sensor Calibration Parity — ✅ shipped in v0.3.3

Stock does per-boot line-fitting calibration on ADC1 and interprets
hall thresholds in millivolts, not raw counts. Our first attempt in
v0.2.4 broke real hardware (LEDs latched red, device hung after motor
commands). We reverted in v0.2.5 and shipped raw-count thresholds
with a wide CLOSED band through the whole 0.3.x line — worked on the
one tester's board but was per-board fragile.

v0.3.3 restores stock parity behind a Ghidra-verified spec:

- [x] Full audit of the stock ADC path: which unit, which scheme,
      exact config values, channel init order, and boot ordering
      relative to LEDC / RMT / WiFi. Result: [`docs/adc-calibration-spec.md`](adc-calibration-spec.md)
- [x] Identified two demonstrable v0.2.4 vs stock differences: LEDC
      configured before ADC unit/cali (we had it backwards), and
      `clk_src` / `bitwidth` left at defaults instead of the explicit
      values stock passes. Root cause of the crash isn't proven from
      disassembly alone, but restoring both invariants works.
- [x] Threshold values documented in mV to match stock's inclusive
      bounds: OPEN 640–960, CLOSED 1360–1680, past-closed 2080–2450.
      Config-detect bands too (1900–2400 mV → 4 groups,
      1100–1700 mV → 2 groups, <200 mV → 0)
- [x] Calibration + channels initialised inside `dv_motor_init` before
      LEDC or any WS2812 code exists. WS2812 driver doesn't ship yet
      (Phase 4), but when it lands it must sequence after
      `dv_motor_init` to preserve the invariant.
- [x] Firmware-transparent: no user config change needed, existing
      NVS still valid, arrival debounce and gave-up flag from v0.2.6
      retained as belt-and-braces

## Phase 4 — RGB Lighting (planned for 0.4.x)

WS2812 status lighting is the biggest remaining gap vs stock. Now
unblocked — Moonraker ingest and hall calibration are stable. Rough
sketch of what's needed:

- WS2812 RMT driver, strip-count auto-detect via GPIO 35 ADC
- Simple mode: 7 canned effects with color/brightness/speed sliders
- Advanced mode: per-state color mapping (six printer states)
- Warning override (flash red on printer error)
- "Follow printer" / "Follow exhaust vent" / "Reverse direction" toggles

> **Ordering invariant** — WS2812 GPIOs (4 / 14) are ADC2 CH0 / CH6.
> Stock initialises ADC1 + line-fitting cali *before* creating any RMT
> channel; DragonVent's `dv_motor_init` also runs first, and the RGB
> component when added must be scheduled after it in `app_main`. See
> [`adc-calibration-spec.md`](adc-calibration-spec.md) §"ADC2 and the
> WS2812 failure" — this ordering is why v0.2.4 latch-red and hangs
> stopped happening in v0.3.3.

## Phase 5 — Extras & Polish

- [ ] Print progress on LEDs (once RGB lands)
- [ ] Klipper macro integration — expose HTTP endpoints so gcode macros
      can explicitly command vent open/close and RGB effects
- [ ] Home Assistant / MQTT bridge (optional fallback for non-Moonraker
      setups)
- [ ] Simplified Chinese portal translation (stock feature; low priority)

## Architecture

Firmware is split into standalone ESP-IDF components under
`firmware/components/`. `app_main` is a thin orchestrator: boot each
service in order, register the button callback, mirror policy mode to
the LED.

```
                    ┌─────────────────────────────┐
    printer ─── ws ──┤  dc_moonraker  (dragon-core)│
                     │  state, temps, material     │
                     └──────────┬──────────────────┘
                                │ status
                                ▼
    button ──►  dv_button ──►  dv_policy ──►  dv_motor  ──► 4x DC motor
                     │        (auto/manual)      │           + hall ADC
                     │              │            │
                     ▼              ▼            │
               dv_status_led    dv_portal ◄──────┘  (product API adapter/callbacks)
               (GPIO 27)             │
                                     ▼
                                  dc_wifi (dragon-core STA + AP fallback)

    dv_board = pin definitions shared by every component
```

Every long-lived component owns its own FreeRTOS task and exposes a
thread-safe API. The shared SPA and `dc_portal` management plane run in both AP
and station modes; product state (WiFi/Moonraker/policy) lives in the
`app_nvs` NVS namespace so it survives reboots and is compatible with
the stock firmware's layout.
