# Panda Vent Hardware Analysis

> Reverse-engineered from the stock firmware binary (`panda_vent_v1.0.0.bin`)
> and official BTT documentation.

## ESP32 Module

The firmware binary confirms this is a **classic ESP32 (Xtensa dual-core LX6)** — NOT an S2/S3/C3.

Evidence from binary strings:
- HAL paths: `hal/esp32/include/hal/gpio_ll.h`, `hal/esp32/include/hal/adc_ll.h`
- Toolchain: `xtensa-esp-elf` (not `riscv32-esp-elf`)
- Port code: `port/esp32/rtc_time.c`, `port/soc/esp32/clk.c`
- Built with **ESP-IDF** v5.3.1 (not Arduino), project name `panda_vent`, compiled 2026-05-27

Firmware image segment layout (from `esptool image-info`):

| Segment | Load addr | Length | Region |
|---------|-----------|--------|--------|
| 0 | 0x3f400020 | 0x3a3c8 | DROM (rodata/strings) |
| 1 | 0x3ff80000 | 0x00004 | RTC DRAM |
| 2 | 0x3ffb0000 | 0x04394 | DRAM (initialized data) |
| 3 | 0x40080000 | 0x01880 | IRAM (vector table etc.) |
| 4 | 0x400d0020 | 0xc0304 | IROM (main code) |
| 5 | 0x40081880 | 0x16e18 | IRAM (main code) |

## GPIO Pin Map

### Confirmed (from documentation)

| Function | GPIO | Peripheral | Source |
|----------|------|-----------|--------|
| **User Button (switch)** | GPIO 12 | GPIO input (pull-up, active-low) | BTT User Manual + stock disassembly |
| **User Button LED**     | GPIO 27 | GPIO output (active-high; off = auto, blink = manual) | Stock disassembly (`gpio_config_t` at 0x3f417100 + LED loop `FUN_400dec04`) |
| **BOOT Button** | GPIO 0 | GPIO input (factory reset, 3 s hold) | BTT user manual + binary |

Long-press threshold: **3 s** for both buttons. Two BTT sources agree —
the v1.0.0 stock binary (`DAT_400d0948 = 2999`, × 10 ms tick) and the
v1.0.0 shipped user manual PDF. The public [BTT wiki](https://neo.bttwiki.com/en/docs/panda-series/module/panda-vent/panda-vent-firmware) says 6 s, but v1.0.0 is the only firmware version BTT ever released — the wiki appears aspirational rather than documenting shipped behavior. We match ground truth.

### Confirmed (from Ghidra disassembly — see `analysis/`)

The firmware defines **4 independent motor groups**, each with its own hall
sensor + forward/reverse LEDC PWM pair. Config lives in a `motor_channel_t[4]`
array in DRAM whose per-group struct we dumped from memory:

| Group | Hall ADC ch → GPIO | Fwd LEDC ch → GPIO | Rev LEDC ch → GPIO |
|-------|--------------------|--------------------|--------------------|
| **0** | ADC1_CH2 → GPIO 38 | LEDC ch 4 → GPIO 22 | LEDC ch 5 → GPIO 21 |
| **1** | ADC1_CH0 → GPIO 36 | LEDC ch 0 → GPIO 25 | LEDC ch 1 → GPIO 26 |
| **2** | ADC1_CH1 → GPIO 37 | LEDC ch 2 → GPIO 32 | LEDC ch 3 → GPIO 33 |
| **3** | ADC1_CH3 → GPIO 39 | LEDC ch 6 → GPIO 23 | LEDC ch 7 → GPIO 19 |

WS2812 RGB output is a `rgb_channels[2]` array; the second entry is only
enabled in the retail 2-vent kit configuration (see auto-detect below).

| Function | GPIO | Peripheral | Evidence |
|----------|------|-----------|----------|
| **Hardware config detect** | ADC1_CH7 (GPIO 35) | ADC oneshot | `adc_oneshot_config_channel(handle, 7, ...)` in `hall_adc_init`; three-band classifier in `FUN_400deb88` |
| **WS2812 Strip 0** | GPIO 14 | RMT TX | `rgb_channels[0].gpio_num = 14` (dumped from 0x3ffb031c) |
| **WS2812 Strip 1** | GPIO 4  | RMT TX | `rgb_channels[1].gpio_num = 4`  (dumped from 0x3ffb0338) |

### ESP32 ADC1 Channel → GPIO Reference

| ADC1 Channel | GPIO |
|-------------|------|
| CH0 | GPIO 36 (SVP) |
| CH1 | GPIO 37 |
| CH2 | GPIO 38 |
| CH3 | GPIO 39 (SVN) |
| CH4 | GPIO 32 |
| CH5 | GPIO 33 |
| CH6 | GPIO 34 |
| CH7 | GPIO 35 |

Note: GPIO 34-39 are input-only on the ESP32.

## Motor Subsystem

Source file paths found in binary:
- `./main/motor/motor.c` — motor control logic
- `./main/motor/motor_adc.c` — hall sensor reading

Key details:
- **4 motor channel groups** (GROUP 1-4), each with independent hall sensor
- **Forward/reverse PWM** via ESP32 LEDC peripheral (`fwd_chan` / `rev_chan`)
- **Gradual startup** (soft-start ramp to prevent mechanical shock)
- Functions: `motor_pwm_init`, `motor_ledc_timer_init`, `hall_adc_init`, `hall_get_state`
- Per-group config struct is 0x24 bytes; fields at offsets 0x14/0x18/0x1c/0x20
  hold fwd/rev LEDC channel + fwd/rev GPIO. Hall ADC channel is at +0x04.

The 4-group layout supports **up to four daisy-chained vent modules**, one
motor per module. Each vent has identical 3-pin JST connectors on both ends
(user-observed), so a chain can be extended by plugging the next module into
the outgoing side of the previous one. The mainboard exposes two 15-pin
connectors — one per chain — that each break out to a 5P (RGB / detect) +
6P (motor signals) + likely a small connector for control. The retail kit
ships two chains' worth of modules that together populate all four motor
groups; the "single-vent" band of the config detect (2 groups) corresponds
to a partial kit or one populated chain.

### Hardware-config auto-detection

ADC1_CH7 (GPIO 35) is not just LED-strip detect — it's a single analog
line that resistor-divides differently based on what's plugged in, and
selects the whole hardware configuration at boot:

| ADC raw range | Motors | RGB strips | LED count |
|---------------|--------|------------|-----------|
| ~1900–2400    | 4      | 2          | 27        |
| ~1100–1700    | 2      | 1          | 16        |
| < 201         | 0      | 0          | 0         |

This means a single Panda Vent unit (one vent + one RGB board plugged
into just one of the two 15P connectors) runs the same firmware with only
2 motor groups active. Groups 0/1 vs. 2/3 mapping to left vs. right
connector — not yet verified.

### Motor drive state machine

Per motor (from `motor.c` decompilation):

- Shared LEDC timer 0, low-speed mode, **30 kHz** PWM, 10-bit resolution
  (max duty = 1023 = 0x3ff)
- **Open** = drive `fwd_chan` (channel at group struct offset +0x14)
- **Close** = drive `rev_chan` (channel at group struct offset +0x18)
- Direction change: stop opposite channel → 500 ms dead-time → fade active
  channel duty 0 → 1023 over 20 ms (`ledc_set_fade_with_time` + `fade_start`)
- Stop: fade active channel duty to 0 over 10 ms, then hard-off both
- Up to 4 retries if hall doesn't confirm the target position

Hall reading (from `motor_adc.c`) in stock **applies factory line-fitting
calibration** via `adc_cali_raw_to_voltage` before classifying — the numeric
thresholds are millivolts, not raw ADC counts (confirmed by the
`adc_cali_raw_to_voltage` and `adc_cali_create_scheme_line_fitting` symbol
references in stock's binary at `3f42a91c` / `3f42a934`).

| Return | Meaning                        | Calibrated mV range (stock)      |
|--------|--------------------------------|----------------------------------|
| 0      | invalid / disconnected         | 0                                |
| 1      | **OPEN endpoint** (fan-on)     | 640–960 mV  (0x280–0x3c0)        |
| 2      | **CLOSED endpoint** (fan-off)  | 1360–1680 mV (0x550–0x690)       |
| 3      | past-closed / over-travel      | `mv + (-2080) < 0x173` (~2080–2450 mV) |
| 4      | in transit (catch-all)         | anything else                    |

> **Note (v0.3.3):** OpenVent now uses the same calibrated-millivolt
> thresholds and boot ordering as stock — see
> [`adc-calibration-spec.md`](adc-calibration-spec.md) for the full
> reproduction contract. Line-fitting cali handle is created once in
> `dv_motor_init` with `atten=DB_12`, `bitwidth=12`, `default_vref=0`,
> and channels 2/1/0/3/7 are configured in that exact order.
>
> The v0.2.4 failure (WS2812 latch-red + hang after motor commands) has
> two demonstrable differences vs stock that we've now corrected: v0.2.4
> configured LEDC **before** creating the ADC unit and cali handle, and
> it left `clk_src` / `bitwidth` at their defaults instead of the
> explicit `ADC_RTC_CLK_SRC_DEFAULT` / `ADC_BITWIDTH_12` stock passes.
> The stock binary contains no evidence that line-fitting cali itself
> touches ADC2 or GPIO 4 / 14, so the root cause of the crash isn't
> proven from disassembly alone — v0.3.3 restores the stock invariants
> and works, which is the best we can do without a scope. Arrival
> debounce (`ARRIVED_DEBOUNCE_TICKS=3`, 30 ms of in-band samples) is
> retained from v0.2.6 as belt-and-braces for boards where the sensor
> curve is noisier than stock's.

Direction of "OPEN" vs "CLOSED" was derived from the main state machine
(`FUN_400de55c`, lines 36–43): stock reads the user's fan-on/off intent
(`DAT_400d0de4`), maps `fan-on → target state 1` (drives `fwd_chan`) and
`fan-off → target state 2` (drives `rev_chan`). Fan-on = the impeller is
pulling air, which physically requires the vent to be **open** — hence
state 1 = OPEN. Field-verified 2026-07-07 (see
[docs/testing/](testing/)).

The main control loop reads the target state (1 or 2, set by user/auto
logic), compares with `hall_get_state()`, and drives the corresponding
direction until the hall reads the matching endpoint.
- LEDC functions used: `ledc_timer_config`, `ledc_channel_config`, `ledc_set_duty`, `ledc_update_duty`, `ledc_set_fade_with_time`, `ledc_fade_start`

## RGB LED Subsystem

Source file paths found in binary:
- `./main/rgb/app_rgb.c` — LED strip control
- `./main/rgb/app_rgb_effect.c` — lighting effects engine

Key details:
- Uses **RMT peripheral** for WS2812 timing (standard ESP-IDF approach)
- Supports **multiple RMT channels**: `rgb_channels[i].channel`, `rgb_channels[i].encoder`
- LED count and strip count are picked from the same GPIO 35 config-detect ADC as the motor group count (see "Hardware-config auto-detection" above)
- 7 effect modes: static, breathing, strobing, wave, marquee, color cycle, rainbow
- Functions: `rgb_init`, `rgb_light_mode`, `rgb_switch`, `rgb_sundry`, `sys_rgb_mode`, `rmt_new_led_strip_encoder`, `rmt_transmit`

## Communication Subsystem (Stock Firmware)

- **WiFi**: STA + AP mode, mDNS hostname (`PandaVent.local`)
- **MQTT**: `bambu_mqtt` module, local connection to Bambu printer
- **UDP Discovery**: `bambu_udp` — SSDP over UDP to discover Bambu printers
- **Web UI**: ESP HTTP Server (`httpd`) with WebSocket support
- **OTA**: HTTPS OTA via web upload
- **Hotspot**: Default SSID `Panda_Vent_XXXX`, password `987654321`, IP `192.168.254.1`

### MQTT Fields Parsed from Bambu Printers

| Field | Description |
|-------|-------------|
| `gcode_state` | Current print state |
| `stg_cur` | Current stage |
| `nozzle_temper` | Nozzle temperature |
| `bed_temper` | Bed temperature |
| `layer_num` | Current layer number |
| `print_error` | Error code |

Push command sent to Bambu printer:
```json
{"pushing": {"sequence_id": "0", "command": "pushall"}}
```

## NVS Storage Keys

Found in binary strings:

| Key | Purpose |
|-----|---------|
| `ssid` | WiFi SSID |
| `password` | WiFi password |
| `access_code` | Bambu printer access code |
| `ap.ssid` | AP mode SSID |
| `language` | UI language |
| `current_light_mode` | Active lighting mode |
| `rgb_light_mode` | RGB mode setting |
| `sys_rgb_mode` | System RGB mode |
| `warn` | Warning state |
| `warning_hot_mode` | Temperature warning mode |
| `warning_overide` | Warning override flag |
| `warning_sw` | Warning switch |
| `set_hostname` | mDNS hostname |

## Firmware Build Info

- NVS namespace: `app_nvs`
- Device name format: `ESP32_%02x%02X%02X` (MAC-based)
- AP SSID prefix: `Panda_Vent_`
- Built with ESP-IDF, cross-compiled on Windows (`HOST-x86_64-w64-mingw32`)
