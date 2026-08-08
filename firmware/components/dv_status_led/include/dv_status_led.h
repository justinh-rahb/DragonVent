#pragma once

// User-button LED (GPIO 27, active-high). Matches stock firmware's mode
// indicator: off in auto mode, blinking in manual mode. The SOLID mode is
// exposed for future callers (e.g. a "device is on / captive portal up"
// hint) but the vent policy itself only uses OFF and BLINK.

#include "esp_err.h"

typedef enum {
    DV_STATUS_LED_OFF,
    DV_STATUS_LED_SOLID,
    DV_STATUS_LED_BLINK,
} dv_status_led_mode_t;

esp_err_t dv_status_led_start(void);
void      dv_status_led_set(dv_status_led_mode_t mode);
