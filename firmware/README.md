# DragonVent Firmware

ESP-IDF v5.3+ project targeting the classic ESP32 in the Bigtreetech Panda Vent.

## First-time setup (macOS)

Use `../tools/idf-build.sh firmware esp32 build` from the repository root for
local builds. The wrapper locates the required Xtensa/RISC-V compiler explicitly,
checks exact component SHAs against `dependencies.lock`, and quarantines stale
Component Manager state before building. CI uses the same entry point.

```sh
brew install cmake ninja dfu-util
git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
# Add to your shell rc:
alias get_idf='. ~/esp/esp-idf/export.sh'
```

Then in each new shell:

```sh
get_idf
```

## Build / flash / monitor

```sh
cd firmware
idf.py set-target esp32   # first time only
idf.py build
idf.py -p /dev/tty.usbserial-* flash monitor
```

`Ctrl-]` exits the monitor.

## CI / releases

GitHub Actions builds the firmware on pushes and pull requests that touch
`firmware/` or workflow files. Release artifacts are produced when a `v*` tag
is pushed, or by running the "Firmware Release" workflow manually with a tag.

### Flashing release artifacts

Each release ships two binaries:

- **`dragonvent-full.bin`** — bootloader + partition table + OTA data +
  app, one file. For first-time flashing over USB.

  ```sh
  python -m esptool --chip esp32 -p /dev/tty.usbserial-* -b 460800 \
    write_flash 0x0 dragonvent-full.bin
  ```

- **`dragonvent-ota.bin`** — app only. Upload from **Device setup →
  Maintenance** on a device that's already running DragonVent. No USB cable
  needed.

`SHA256SUMS` next to them if you want to verify.

## Layout

```
firmware/
├── CMakeLists.txt
├── partitions.csv           # OTA-capable 4MB layout (two app slots + two NVS)
├── sdkconfig.defaults       # checked in; sdkconfig itself is generated + gitignored
├── main/
│   └── app_main.c           # thin orchestrator: init components, route buttons
└── components/
    ├── dv_board/            # GPIO pin map — single source of truth
    ├── dv_motor/            # 30 kHz LEDC PWM + hall ADC state machine (4 groups)
    ├── dv_button/           # USER + BOOT debouncing, short/long press dispatch
    ├── dv_status_led/       # user-button LED: off = auto, blink = manual
    ├── dv_policy/           # auto/manual mode, hysteresis-based open/close decision
    └── dv_portal/           # API-v2/Tasmota adapter + product setup callbacks
```

Board-neutral WiFi, source selection, Bambu LAN, Moonraker, event-log, the
capability-aware Dragon-family SPA, and its provisioning/recovery service are
pinned by exact commit in `main/idf_component.yml` and fetched from
[`dragon-core`](https://github.com/justinh-rahb/dragon-core). Product-specific
`dv_*` components remain local for now. Components persist compatible state
under the `app_nvs` namespace, so this refactor does not wipe existing settings.

`/` serves the DragonVent surface from `dc_ui` in both station and captive-AP
modes. Shared `dc_portal` owns Wi-Fi scanning and credentials, fallback-AP
configuration, OTA, logs, reset, captive DNS, and recovery routing. The small
product adapter exposes API v2 and Tasmota routes and supplies a schema plus
callbacks for printer-source and vent-policy settings. `/setup` opens the same
SPA overlay; there is no second server-rendered interface.

## Flashing the first time

The stock firmware doesn't expose a UART bootloader path publicly, but the
ESP32 module inside the Panda Vent has the standard EN/BOOT pads. Refer to
BTT's docs for pad locations. Ground BOOT while pulsing EN to enter download
mode, then flash normally.
