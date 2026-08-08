# DragonVent Firmware

ESP-IDF v5.3+ project targeting the classic ESP32 in the Bigtreetech Panda Vent.

## First-time setup (macOS)

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

- **`dragonvent-ota.bin`** — app only. Upload via the portal's
  **OTA firmware update** form on a device that's already running
  DragonVent. No USB cable needed.

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
    └── dv_portal/           # unified web UI (AP + STA), captive DNS in AP mode
```

Board-neutral WiFi, source selection, Bambu LAN, Moonraker, and event-log
services are pinned by exact commit in `main/idf_component.yml` and fetched from
[`dragon-core`](https://github.com/justinh-rahb/dragon-core). Product-specific
`dv_*` components remain local for now. Components persist compatible state
under the `app_nvs` namespace, so this refactor does not wipe existing settings.

## Flashing the first time

The stock firmware doesn't expose a UART bootloader path publicly, but the
ESP32 module inside the Panda Vent has the standard EN/BOOT pads. Refer to
BTT's docs for pad locations. Ground BOOT while pulsing EN to enter download
mode, then flash normally.
