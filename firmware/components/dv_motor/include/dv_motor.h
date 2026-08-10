#pragma once

// Panda Vent motor driver: LEDC PWM + hall-ADC position feedback for up to 4
// independent motor groups. Public API is thread-safe; a single internal task
// runs the per-group state machine.
//
// Behavior mirrors the stock firmware: 30 kHz PWM, soft-start via LEDC fade,
// 500 ms dead-time between direction reversals, up to 4 retries when the hall
// doesn't confirm the target position.

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DV_MOTOR_TARGET_STOP = 0,    // idle, no drive
    DV_MOTOR_TARGET_CLOSED,      // drive rev until hall reads CLOSED
    DV_MOTOR_TARGET_OPEN,        // drive fwd until hall reads OPEN
} dv_motor_target_t;

// Discrete hall-sensor state. Values match stock's classifier so the state
// numbers you see here are the same ones the Ghidra decomp of hall_get_state
// (FUN_400deb24) returns. Physical mapping is derived from stock's fan-on ↔
// state 1 = OPEN, fan-off ↔ state 2 = CLOSED chain in the main state machine
// (FUN_400de55c lines 36-43).
typedef enum {
    DV_HALL_INVALID  = 0,        // sensor disconnected / calibrated value == 0 mV
    DV_HALL_OPEN     = 1,        // at OPEN endpoint   (640-960 mV inclusive)
    DV_HALL_CLOSED   = 2,        // at CLOSED endpoint (1360-1680 mV inclusive)
    DV_HALL_MID_LOW  = 3,        // past-closed / over-travel (2080-2450 mV inclusive)
    DV_HALL_MID_HIGH = 4,        // catch-all — in transit between endpoints
} dv_motor_hall_t;

// Initialize ADC1 + line-fitting calibration before LEDC, sample the calibrated
// config-detect ADC (GPIO 35) to determine the connected motor groups, bring
// those groups online, and spawn the driver task. The task continues to sample
// config detect once per second and dynamically add/remove hot-plugged groups.
esp_err_t dv_motor_init(void);

// Set the target state for one group. group index in [0, active_groups).
esp_err_t dv_motor_set_target(int group, dv_motor_target_t target);

// Get the last hall reading for one group. Non-blocking; may be a stale
// cached value between task ticks.
dv_motor_hall_t dv_motor_hall(int group);

// True while a group is actively driving toward its target.
bool dv_motor_is_running(int group);

// Currently active group count (0/2/4), as most recently determined by the
// hardware-config detect ADC.
int dv_motor_active_groups(void);
