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

typedef struct {
    bool    enabled;         // master on/off
    uint8_t brightness;      // 0-255 global scale
    uint8_t open[3];         // RGB when open  (default blue = cool)
    uint8_t closed[3];       // RGB when closed (default red = hot)
    uint8_t printing[3];     // RGB while the printer is printing
    bool    use_printing;    // apply the printing color while printing
    bool    use_temp;        // blend open->closed by bed/chamber temp
    uint8_t temp_min_c;      // temp at the "cool" end of the gradient
    uint8_t temp_max_c;      // temp at the "hot" end
} dv_lighting_t;

// Detect the connected strips, load saved config, drive the initial color.
esp_err_t dv_rgb_start(void);

// Recompute the strip color from current state and apply it. target is a
// dv_motor_target_t; bed_temp_c may be NAN when no printer/telemetry.
void dv_rgb_update(int target, bool printing, float bed_temp_c);

// Get / set the lighting config. set persists to NVS and re-applies immediately.
void      dv_rgb_get_config(dv_lighting_t *out);
esp_err_t dv_rgb_set_config(const dv_lighting_t *cfg);

// Number of strips currently driven (0/1/2).
int dv_rgb_strip_count(void);
