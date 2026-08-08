#pragma once

// Vent policy: consumes Moonraker status, commands the motor driver. Owns the
// AUTO/MANUAL mode toggle and the temperature hysteresis for auto decisions.
//
// AUTO mode:
//   OPEN when the printer is printing OR the bed is above the "hot" threshold
//   CLOSED when the printer is idle AND the bed is below the "cold" threshold
//   (between = keep current target — hysteresis)
//
// MANUAL mode:
//   target = whatever dv_policy_set_manual_target set most recently

#include <stdbool.h>
#include "esp_err.h"
#include "dv_motor.h"

typedef enum {
    DV_POLICY_MODE_AUTO,
    DV_POLICY_MODE_MANUAL,
} dv_policy_mode_t;

esp_err_t dv_policy_start(void);

esp_err_t dv_policy_set_mode(dv_policy_mode_t mode);   // persisted to NVS
dv_policy_mode_t dv_policy_get_mode(void);

esp_err_t dv_policy_set_manual_target(dv_motor_target_t t);   // persisted
dv_motor_target_t dv_policy_get_target(void);          // whatever we're commanding

// Bed-temperature hysteresis for AUTO mode (idle/complete only — during a
// print the material rule wins). Default 45 / 35 °C. OPEN must be strictly
// greater than CLOSE.
esp_err_t dv_policy_get_thresholds(float *bed_open_c, float *bed_close_c);
esp_err_t dv_policy_set_thresholds(float bed_open_c, float bed_close_c);

esp_err_t dv_policy_clear(void);   // wipe all persisted policy state
