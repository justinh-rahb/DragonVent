# Changelog

All notable changes to the **DragonVent** firmware are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are the
firmware release tags (`vX.Y.Z`). The release workflow pulls the matching section
below into the GitHub Release notes.

## [Unreleased]

## [0.5.8] - 2026-08-19

### Added
- **Seal the vent while the chamber heater is heating.** In AUTO, DragonVent now
  closes the vent whenever a paired **DragonBreath** (via the `dragonbreath-klipper`
  helper) is deliberately heating — `connected && !fault && !inhibited &&
  device_target > 0 && mode ∈ {power_on, auto}`. This covers a chamber heat soak,
  where a warm bed at idle would otherwise open the vent to shed residual heat.
  Uses the helper's confirmed state over Moonraker (re-pins **dragon-core v0.25.0**);
  no extra configuration. Turning the chamber heater off returns to the normal
  bed/material policy. Hardware-validated end-to-end.

## [0.5.7] - 2026-08-19

### Fixed
- **Moonraker connection can no longer silently die.** A half-open WebSocket used to
  leave the vent "connected" but frozen on stale printer data. Re-pins **dragon-core
  to v0.24.0**, which adds WebSocket ping/pong plus a staleness watchdog that
  reconnects and re-subscribes if no update arrives for ~45 s. Hardware-validated
  across a printer-host reboot.

## [0.5.6] - 2026-08-18

### Changed
- **Adopt the shared `dc_lighting` engine** (dragon-core), ending the duplicate LED
  effect code between DragonVent and DragonStatus. `dv_rgb` is now a thin adapter that
  keeps the vent's color policy and delegates rendering to the shared engine. Per-strip
  reverse and the state-colored Cylon effect are preserved; SPI+DMA transport and the
  per-strip in-sync layout are unchanged.

## [0.5.5] - 2026-08-14

### Added
- **Per-strip LED reverse.** Each WS2812 strip's direction is independent (Reverse
  strip 1 / 2), so a vent whose strips are fed from opposite connectors can run them
  the same way or opposite — letting effects "circle" the printer.
- **Cylon eye** as a distinct effect (previously it fell through to Marquee).

## [0.5.4] - 2026-08-11

### Fixed
- Enable the esp-tls options the Bambu LAN client requires so **Bambu LAN can connect**
  (re-pins dragon-core to v0.14.0).

## [0.5.3] - 2026-08-11

### Fixed
- Clean re-cut of 0.5.2: the build now stamps a clean version and `dependencies.lock`
  is untracked (fixes the "-dirty" release version).

## [0.5.2] - 2026-08-11

### Added
- **On-demand Bambu LAN discovery** in setup (SSDP scan + printer picker).

### Fixed
- **Rainbow flicker** on the classic ESP32 — drive WS2812 over SPI+DMA instead of RMT,
  whose refill ISR could be starved mid-frame.

## [0.5.1] - 2026-08-11

### Added
- **RGB status lighting** — WS2812 effects (Strobe/Wave/Marquee, …), reverse LED
  direction, and per-printer-status colors (Follow Printer mode).

### Changed
- **OTA-only install** — dropped the `factory.bin` download and de-referenced the USB
  helper from the release.

## [0.5.0] - 2026-08-11

### Added
- **Install over stock, no USB.** DragonVent runs on the stock Panda Vent partition
  table, so it installs and updates entirely from the web UI through the stock
  firmware's own OTA — the stock bootloader is preserved and you can revert to stock.
