#pragma once

// WS2812 addressable strip LEDs (GPIO 14, and GPIO 4 on 2-vent kits). Strip and
// LED count follow the same GPIO-35 config-detect as the motor groups:
//   2 motor groups -> 1 strip (16 LEDs);  4 groups -> 2 strips (27 LEDs).
//
// IMPORTANT ordering: initialize this AFTER dv_motor (which creates the ADC1
// oneshot + line-fitting cal, then the LEDC timer). Stock brings up ADC first,
// then LEDC, then the RMT/WS2812 channels last; bringing RMT up before the ADC
// cal is what latched the strips red and hung the board in OpenVent v0.2.4.

#include "esp_err.h"
#include <stdint.h>

// Detect the connected strips and drive them to an initial solid color.
esp_err_t dv_rgb_start(void);

// Set every LED on every active strip to one RGB color (0-255 each), applied
// immediately. Thread-safe against other dv_rgb_set callers.
esp_err_t dv_rgb_set(uint8_t r, uint8_t g, uint8_t b);

// Number of strips currently driven (0/1/2).
int dv_rgb_strip_count(void);
