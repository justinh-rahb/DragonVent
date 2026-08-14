#pragma once

// WS2812 addressable strip LEDs (GPIO 14, and GPIO 4 on 2-vent kits) plus the
// lighting policy that decides their color.
//
// IMPORTANT ordering: initialize this AFTER dv_motor (which creates the ADC1
// oneshot + line-fitting cal, then the LEDC timer). Stock brings up ADC first,
// then LEDC, then the RMT/WS2812 channels last; bringing RMT up before the ADC
// cal is what latched the strips red and hung the board in OpenVent v0.2.4.
//
// Color policy (highest precedence first), all layers user-configurable:
//   1. lighting disabled            -> off
//   2. printing (if use_printing)   -> printing color
//   3. bed temp (if use_temp)       -> gradient: open color (cool) -> closed color (hot)
//   4. otherwise                    -> vent state: open color / closed color
//   then scaled by global brightness.

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Animated effects. SOLID uses the state color (open/closed/printing/temp); the
// others animate and ignore the state color except BREATHE, which pulses it.
typedef enum {
    DV_FX_SOLID = 0,   // steady, state-based color
    DV_FX_CYCLE,       // whole strip cycles through the hue wheel together
    DV_FX_RAINBOW,     // rainbow spread across the strip, scrolling
    DV_FX_BREATHE,     // state color with a breathing brightness pulse
    DV_FX_STROBE,      // state color flashing on/off
    DV_FX_WAVE,        // brightness wave of the state color travelling the strip
    DV_FX_MARQUEE,     // theatre-chase of the state color
    DV_FX_CYLON,       // bouncing "eye" (Larson scanner) of the state color
} dv_light_fx_t;

// How the base color is chosen.
typedef enum {
    DV_LIGHT_MODE_VENT    = 0,  // by vent state: open / closed color (+ temp gradient)
    DV_LIGHT_MODE_PRINTER = 1,  // by printer status: idle/prep/printing/paused/complete/error
} dv_light_mode_t;

// Printer status fed to the lighting policy (mapped from the active source).
typedef enum {
    DV_PS_NONE = 0,   // no printer / standalone
    DV_PS_IDLE,
    DV_PS_PREPARING,
    DV_PS_PRINTING,
    DV_PS_PAUSED,
    DV_PS_COMPLETE,
    DV_PS_ERROR,
} dv_printer_status_t;

// NOTE: append new fields at the END — cfg_load tolerates a shorter stored blob
// (older versions keep their values; new fields fall back to these defaults).
typedef struct {
    bool    enabled;         // master on/off
    uint8_t brightness;      // 0-255 global scale
    uint8_t open[3];         // RGB when open  (default blue = cool)
    uint8_t closed[3];       // RGB when closed (default red = hot)
    uint8_t printing[3];     // RGB while printing (vent-mode override + printer-mode status)
    bool    use_printing;    // vent mode: apply the printing color while printing
    bool    use_temp;        // vent mode: blend open->closed by bed/chamber temp
    uint8_t temp_min_c;      // temp at the "cool" end of the gradient
    uint8_t temp_max_c;      // temp at the "hot" end
    uint8_t effect;          // dv_light_fx_t
    uint8_t speed;           // 0-255 animation speed (effect != SOLID)
    uint8_t error[3];        // RGB shown (flashing) on a print error
    bool    use_error;       // flash the error color on a print error
    uint8_t mode;            // dv_light_mode_t: vent state vs printer status
    uint8_t idle[3];         // printer-status colors (mode == PRINTER):
    uint8_t prep[3];
    uint8_t paused[3];
    uint8_t complete[3];
    bool    reverse;         // legacy global reverse; migrated into rev_strip[] on load
    uint8_t rev_strip[2];    // per-strip full reverse (0/1): flip a strip's whole
                             // direction. Use when a strip is fed from the far
                             // connector so it runs opposite the other (e.g. lights
                             // "circle" the printer). Indexed by s_strips[] order.
} dv_lighting_t;

// Detect the connected strips, load saved config, drive the initial color.
esp_err_t dv_rgb_start(void);

// Feed current state to the lighting policy. target is a dv_motor_target_t;
// status is a dv_printer_status_t (drives printer-mode color + the error flash);
// bed_temp_c may be NAN when there's no printer/telemetry.
void dv_rgb_update(int target, int status, float bed_temp_c);

// Get / set the lighting config. set persists to NVS and re-applies immediately.
void      dv_rgb_get_config(dv_lighting_t *out);
esp_err_t dv_rgb_set_config(const dv_lighting_t *cfg);

// Number of strips currently driven (0/1/2).
int dv_rgb_strip_count(void);
