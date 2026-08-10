# Task: Stock Panda Vent ADC Calibration Audit

**Type:** Reverse-engineering / analysis. **Not a coding task.**
**Deliverable:** A short markdown spec describing exactly what stock does, so
we can reproduce it in OpenVent without repeating the v0.2.4 failure.

**Repo:** `justinh-rahb/OpenVent` (the working tree you're reading this from
is also the working tree you'll do the audit in). Everything you need — the
stock binary segments, PyGhidra pipeline, existing decompiles — is checked in
under `analysis/`.

---

## Why we're doing this

OpenVent replaces the stock Bigtreetech Panda Vent firmware. The stock hall
sensor code applies **factory line-fitting ADC calibration** before comparing
readings to thresholds in **millivolts**. OpenVent's current shipping firmware
(v0.3.1) instead uses **raw ADC counts** with widened bands and a 30 ms
arrival debounce — that works on the one board we've tested, but it's a
per-board gamble because ESP32 ADCs vary by ~5–10 % between chips.

We want parity with stock, but a naive attempt in v0.2.4 broke real hardware
(see [Failure below](#the-v024-failure-we-need-to-understand)). Before writing
any calibration code again, we need to know exactly what stock does.

## What we already know

Documented in [`docs/HARDWARE_ANALYSIS.md`](../HARDWARE_ANALYSIS.md):

- **Hall ADC channels:** ADC1 CH0 / CH1 / CH2 / CH3 on GPIO 36 / 37 / 38 / 39
  (one per motor group). All input-only pins.
- **Extra ADC channel:** CH7 on GPIO 35 — the "config-detect" line, three-way
  classifier for kit configuration (populated by `hall_adc_init`).
- **Calibrated thresholds from stock's `hall_get_state`:**
  - `1 = OPEN`   → 640–960 mV
  - `2 = CLOSED` → 1360–1680 mV
  - `3 = past-closed` → ~2080–2450 mV
  - `4 = in transit`
- **Calibration symbols referenced in the binary:**
  - `adc_cali_raw_to_voltage` — pointer at `0x3f42a91c`
  - `adc_cali_create_scheme_line_fitting` — pointer at `0x3f42a934`
- **RGB output shares the ADC2 pin domain:** WS2812 strips are on GPIO 4
  (ADC2 CH0) and GPIO 14 (ADC2 CH6). This matters for the failure mode below.

## The v0.2.4 failure we need to understand

When OpenVent v0.2.4 initialized `adc_cali_create_scheme_line_fitting` at
boot on real Panda Vent hardware:

1. **WS2812 strips on GPIO 4 / GPIO 14 latched red at boot** — never
   transitioned to any commanded state.
2. **The board hung after repeated motor commands** — required USB replug to
   recover.

Working theory: the calibration init disturbed shared ADC/RTC state that the
RMT-driven WS2812 pins also depend on. But we do not actually know:

- Whether stock initialises calibration before or after RMT strip init.
- Whether stock uses `line_fitting` at all vs `curve_fitting` or a different
  scheme.
- Whether stock calibrates once per boot or on demand.
- Whether stock uses ADC oneshot or continuous mode for the hall reads.
- What the `-DAT_400d0dcc` and `PTR_DAT_400d0df4` inputs to `hall_adc_init`
  are (they configure the ADC unit + line-fitting scheme).

Guessing wrong ships another dead board to a tester. We don't want to do that.

## Deliverables

A new file `docs/adc-calibration-spec.md` that answers, at minimum:

1. **Which ADC unit** stock uses for the hall reads (ADC1, expected — confirm).
2. **Which calibration scheme** stock creates. Exact function called with
   exact arguments — atten level, bit width, unit id. Reference the caller in
   the disassembly by name/address.
3. **When calibration is initialised** relative to:
   - LEDC (PWM) init
   - RMT / WS2812 strip init
   - WiFi / MQTT bring-up
   - The main task/loop entry
   Provide the call order in `app_main` (or wherever bring-up lives).
4. **Whether calibration is per-boot only, or re-run on some event** (e.g.
   temperature drift, error recovery).
5. **How `hall_get_state` uses the calibration handle** — does it convert
   every read to mV via `adc_cali_raw_to_voltage`, or is there a cached
   linear fit it applies inline?
6. **Exact millivolt thresholds** used in `hall_get_state`'s classifier (we
   have approximate ranges from prior work — confirm or correct).
7. **Any interaction with GPIO 4 / GPIO 14 (ADC2 CH0 / CH6)** — does stock
   touch ADC2 at all? Does it install any handler or hold GPIOs during
   calibration init that could explain the WS2812 latch-red we observed?
8. **The three-way classifier bands for the config-detect line** (ADC1 CH7 /
   GPIO 35) that decide 0 / 2 / 4 motor groups. Also in millivolts.

Where behaviour is ambiguous or you had to infer, say so and cite the
disassembly you inferred from.

## How to work

Everything is committed. From the repo root:

```
# One-time setup
python3 -m venv analysis/venv
analysis/venv/bin/pip install pyghidra
brew install ghidra   # or install Ghidra 12.1.2 however you prefer

# Fire the pipeline (rebuilds the Ghidra project if needed)
analysis/venv/bin/python3 analysis/run_pyghidra.py
```

The pipeline is set up to import the stock binary segments (`analysis/segments/`)
at their real load addresses, run auto-analysis, and drop decompiles into
`analysis/decomp/`. To add functions to the pipeline, edit the `TARGETS` list
in [`analysis/run_pyghidra.py`](../../analysis/run_pyghidra.py).

Functions you'll want in there for this audit (existing ones plus new):

- `hall_adc_init` — already decompiled, at `0x400dea18`
- `hall_get_state` — **not yet decompiled**, this is where the calibration is
  applied per-read
- `app_main` — bring-up order
- `rgb_init` — already decompiled, need to see relative init order vs hall
- Any function that references `adc_cali_create_scheme_line_fitting`
  (`PTR_DAT_400d0df4` in the current decomp is the pointer table for it —
  find callers using `analysis/find_callers.py`)

The prior probe scripts (`probe_motor.py`, `probe_led.py`, …) are worked
examples of how to poke around specific subsystems with PyGhidra; copy the
pattern.

## What NOT to do

- **Don't touch firmware code.** No changes under `firmware/`. If the spec
  reveals we can safely retry calibration, a separate follow-up task will
  implement it.
- **Don't flash anything.** Analysis only.
- **Don't guess.** If the disassembly doesn't tell you, say "unknown" in the
  spec. Wrong confident answers are worse than "unknown."

## Success criteria

- `docs/adc-calibration-spec.md` exists and is committed.
- A reader who's never seen the stock binary can, from that doc alone, write
  ESP-IDF code that mirrors stock's calibration init (correct scheme, correct
  order, correct thresholds) with no further reverse-engineering.
- No open questions marked "unknown" for items 1–3 or 6 (the ones that
  actually gate whether we can retry safely). Items 4, 5, 7, 8 can remain
  partially unknown if the disassembly genuinely doesn't answer them —
  document what you tried.

## References

- Prior hardware audit: [`docs/HARDWARE_ANALYSIS.md`](../HARDWARE_ANALYSIS.md)
- Roadmap Phase 3 context: [`docs/ROADMAP.md`](../ROADMAP.md#phase-3--hall-sensor-calibration-parity)
- OpenVent's current hall code (raw counts, widened bands): [`firmware/components/dv_motor/dv_motor.c`](../../firmware/components/dv_motor/dv_motor.c)
- Stock binary segments: `analysis/segments/`
- Existing decompiles: `analysis/decomp/`
- PyGhidra pipeline: `analysis/run_pyghidra.py`
