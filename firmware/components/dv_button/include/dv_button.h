#pragma once

// User (GPIO 12) + BOOT (GPIO 0) button handler. Both are active-low inputs
// with internal pull-ups. Debounce is 10 ms; long-press threshold is 3 s to
// match stock v1.0.0 (the only released version) — see the note in
// dv_button.c on the wiki-vs-binary discrepancy.

#include "esp_err.h"

typedef enum {
    DV_BUTTON_USER,
    DV_BUTTON_BOOT,
} dv_button_id_t;

typedef enum {
    DV_BUTTON_SHORT,      // pressed and released before the long-press threshold
    DV_BUTTON_LONG,       // held past the threshold (fires once, at the threshold)
} dv_button_event_t;

typedef void (*dv_button_cb_t)(dv_button_id_t id, dv_button_event_t ev);

// Register a single callback that receives every button event, then spawn the
// polling task. The callback runs on the button task; keep it short and
// non-blocking.
esp_err_t dv_button_start(dv_button_cb_t cb);
