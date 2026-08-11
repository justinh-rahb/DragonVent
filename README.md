# DragonVent

Open firmware for the [BIGTREETECH Panda Vent](https://github.com/bigtreetech/Panda-Vent), built on the shared [`dragon-core`](https://github.com/justinh-rahb/dragon-core) networking and printer-integration components.

## What is this?

The Panda Vent is a smart vent riser for enclosed 3D printers. DragonVent replaces its stock firmware and currently integrates with Klipper printers through Moonraker while preserving the proven OpenVent motor, hall-sensor, button, and vent-policy implementation.

## Features

Working today (OpenVent v0.3.3 baseline plus the in-development DragonVent refactor):

- **Automatic vent control** — six-state printer model (idle / preparing / printing / paused / complete / error), material-aware policy (PLA opens for cooling, ABS/ASA seals for heat retention), bed-temp hysteresis for residual heat
- **Stock-parity hall sensing** — per-boot ADC line-fitting calibration with calibrated-millivolt thresholds, matching stock's reproduction contract
- **Dragon-family dashboard** — responsive DragonVent-flavoured control UI for live vent/source state, manual open/close, and automatic threshold policy
- **Unified browser management** — the shared Dragon SPA is used on the LAN and on the captive setup AP, with integrated Wi-Fi, printer-source, OTA, logs, and reset controls
- **Moonraker integration** — WebSocket ingest with `webhooks` / `print_stats` / `virtual_sdcard` / `heater_bed` / `extruder` / optional chamber + `save_variables` (for material), re-subscribes on Klippy restart
- **Bambu LAN integration (experimental)** — optional read-only MQTT source using the printer's LAN access code; DragonVent never sends printer-control commands. The shared client and portal path build cleanly, but still need validation against a real Bambu printer
- **Single-source binding** — select Klipper, Bambu LAN, or standalone mode; only the selected printer client starts
- **Configurable thresholds** — bed OPEN/CLOSE °C editable in the portal, persisted to NVS
- **Physical button control** — auto/manual mode toggle, manual vent override, manual target persists across reboots
- **Schema-driven setup** — product-specific source and vent-policy fields are rendered by the common SPA instead of a second firmware page
- **OTA firmware updates** — flash new firmware from the web UI

Deferred:

- **RGB LED status** — WS2812 driver + effects engine, planned for the 0.4.x series

## Documentation

- [Hardware Analysis](docs/HARDWARE_ANALYSIS.md) — reverse-engineered GPIO pinout and hardware details
- [Roadmap](docs/ROADMAP.md) — development phases and architecture

## Hardware

- **Kit contents**: 1 mainboard + several motorized vent modules + LED boards. Each vent module has one motor, one hall sensor, and identical 3-pin JST connectors on both ends — so multiple modules chain together
- **Board**: Bigtreetech Panda Vent (ESP32 Xtensa dual-core LX6)
- **Motors**: up to 4 independent DC motors across two mainboard chains, each driven forward/reverse via LEDC PWM at 30 kHz with hall-sensor position feedback
- **LEDs**: WS2812 addressable strips via RMT — GPIO 14 and GPIO 4, one per chain
- **User button**: switch on GPIO 12, illumination LED on GPIO 27 (off = auto, blink = manual)
- **BOOT button**: GPIO 0 (long-press = factory reset)
- **Hardware auto-detect**: single ADC on GPIO 35 picks between "all chains populated" (4 motors), "one chain" (2 motors), and "nothing" — hot-plug supported

Full pin map + provenance: [docs/HARDWARE_ANALYSIS.md](docs/HARDWARE_ANALYSIS.md).

## Install — over stock, from a browser (v0.5.0+)

As of **v0.5.0** DragonVent runs on the **stock Panda Vent partition table**, so
you install and update it **entirely from the web UI** — no serial cable, no
helper scripts. The stock bootloader is preserved; only the app slot is written.

1. Grab `dragonvent-<tag>-ota.bin` from the [latest release](../../releases/latest).
2. Open the **stock** Panda Vent's web UI (`http://PandaVent.local/` or its IP)
   and upload the `ota.bin` on its firmware-update page. It writes DragonVent to
   the inactive slot and reboots into it.
3. First boot is the `DragonVent_XXXX` setup AP (WPA2, password `987654321`);
   join it to set WiFi + printer. Afterwards it's on mDNS at `DragonVent.local`.

Updating an existing DragonVent is the same file, from **Device setup →
Maintenance** in the DragonVent web UI.

**⚠ Back up stock first** — BTT publishes no Panda Vent image, so dump the flash
over USB *before* your first install; it's your only way back:

```
python -m esptool --chip esp32 -p PORT -b 460800 read_flash 0x0 0x400000 stock-panda-vent-backup.bin
```

**Upgrading from 0.4.x:** those builds used a different partition table, so you
can't OTA straight across. Write your stock backup back over USB, then install
over stock from the web UI (same one-time roll-back as the DragonBreath 1.0
migration). 0.5.0+ update in place.

```
python -m esptool --chip esp32 -p PORT -b 460800 write_flash 0x0 stock-panda-vent-backup.bin
```

## Status

**DragonVent is the continuation of OpenVent.** The first refactor keeps the
v0.3.3 hardware behavior and NVS layout while moving WiFi, Moonraker, and event
logging onto pinned `dragon-core` components.

**OpenVent v0.3.3 restored stock ADC calibration parity.** Reverse-engineered
against the Ghidra decompile of stock v1.0.0 —
see [`docs/adc-calibration-spec.md`](docs/adc-calibration-spec.md) for
the reproduction contract. This unblocks per-board hall-sensor accuracy
without requiring per-board threshold tuning.

Prior milestone: **v0.2.6 was the first stable proof-of-concept release.**
2026-07-10 field test on tester OldGuyMeltsPlastic's retail 2-vent kit:
10× consecutive open/close cycles, no ESP crash, motor stops cleanly on
each arrival.

- ✅ Motors drive both directions and reliably stop at endpoints, using
  stock-parity mV thresholds and per-boot ADC line-fitting calibration
- ✅ Six-state printer model + material-aware auto policy, re-subscribes
  on Klippy restart
- ✅ Firmware flashes on real Panda Vent hardware; `dragonvent` script for
  backup / restore / install works end-to-end
- ✅ WiFi station + AP fallback, mDNS `DragonVent.local`, captive portal
- ✅ Unified SPA management: live vent controls plus schema-driven Wi-Fi,
  printer-source, fallback-AP, event-log, OTA, and factory-reset setup
- ⬜ Deferred to 0.4.x: WS2812 RGB status lighting

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/wildtang3nt)

## License

[MIT](LICENSE) © Justin Hayes
