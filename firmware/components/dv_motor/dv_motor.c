#include "dv_motor.h"
#include "dv_board.h"

#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "dv_motor";

// PWM configuration — matches stock firmware.
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_RES_BITS       LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ        30000
#define DUTY_MAX            ((1u << 10) - 1u)   // 1023
#define DUTY_KICK           0x66u               // ~10% initial kick before fade
#define FADE_UP_MS          20                  // 0→full over 20 ms
#define FADE_DOWN_MS        10                  // full→0 over 10 ms
#define DEAD_TIME_MS        500                 // wait between direction reversals
#define VERIFY_TIMEOUT_MS   200                 // hall must confirm within this window
#define RETRY_PAUSE_MS      50                  // brief stop between retries
#define MAX_RETRIES         4
#define TICK_MS             10                  // task loop period

// Stock hall thresholds. These are calibrated millivolts, not ADC counts.
// The unsigned width tests preserve stock's inclusive upper bounds.
#define HALL_OPEN_LO_MV        0x280   // 640 mV
#define HALL_OPEN_WIDTH_MV     0x141   // through 960 mV inclusive
#define HALL_CLOSED_LO_MV      0x550   // 1360 mV
#define HALL_CLOSED_WIDTH_MV   0x141   // through 1680 mV inclusive
#define HALL_PAST_CLOSED_LO_MV 2080
#define HALL_PAST_CLOSED_WIDTH 0x173   // through 2450 mV inclusive
#define ARRIVED_DEBOUNCE_TICKS 3       // 30 ms of continuous in-band samples

// Stock config-detect thresholds, also calibrated millivolts. Readings in the
// deliberate gaps mean "keep current config".
#define DETECT_TWO_LO_MV    0x76c   // 1900 mV
#define DETECT_TWO_WIDTH_MV 0x1f5   // through 2400 mV inclusive
#define DETECT_ONE_LO_MV    0x44c   // 1100 mV
#define DETECT_ONE_WIDTH_MV 0x259   // through 1700 mV inclusive
#define DETECT_NONE_HI_MV   0xc9    // 0-200 mV inclusive

// Config detect cadence: sample every 100 ticks (~1 s) and require the band
// to hold for DEBOUNCE_CYCLES consecutive samples before we act on it.
#define DETECT_INTERVAL_TICKS   100
#define DETECT_DEBOUNCE_CYCLES  3

typedef enum {
    DIR_NONE = 0,
    DIR_FWD,
    DIR_REV,
} dir_t;

typedef struct {
    dv_motor_target_t target;      // last commanded target (mutated by API)
    dv_motor_target_t applied;     // target the state machine is currently driving toward
    dir_t             dir;         // channel currently energised
    bool              running;
    int               retries;
    int               arrived_consec;      // consecutive ticks reading target
    bool              gave_up;             // exhausted retries; wait for new target
    TickType_t        drive_started_tick;
    dv_motor_hall_t   hall_cached;
} group_state_t;

static int              s_active_groups = 0;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t          s_adc_cali   = NULL;
static group_state_t    s_groups[DV_MOTOR_GROUP_COUNT];
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t     s_task = NULL;

// Config-detect state — only touched from the motor task.
static int s_detect_last_band  = -1;
static int s_detect_streak     = 0;
static int s_detect_countdown  = 0;

// ---------- helpers ----------

static inline ledc_channel_t chan_of(int g, dir_t d)
{
    return d == DIR_FWD ? DV_MOTOR_GROUPS[g].fwd_ledc_ch
                        : DV_MOTOR_GROUPS[g].rev_ledc_ch;
}

static inline dir_t dir_for_target(dv_motor_target_t t)
{
    if (t == DV_MOTOR_TARGET_OPEN)   return DIR_FWD;
    if (t == DV_MOTOR_TARGET_CLOSED) return DIR_REV;
    return DIR_NONE;
}

static inline dv_motor_hall_t hall_for_target(dv_motor_target_t t)
{
    if (t == DV_MOTOR_TARGET_OPEN)   return DV_HALL_OPEN;
    if (t == DV_MOTOR_TARGET_CLOSED) return DV_HALL_CLOSED;
    return DV_HALL_INVALID;
}

static dv_motor_hall_t classify_hall_mv(int mv)
{
    if (mv == 0) return DV_HALL_INVALID;
    if ((uint32_t)(mv - HALL_CLOSED_LO_MV) < HALL_CLOSED_WIDTH_MV) {
        return DV_HALL_CLOSED;
    }
    if ((uint32_t)(mv - HALL_OPEN_LO_MV) < HALL_OPEN_WIDTH_MV) {
        return DV_HALL_OPEN;
    }
    if ((uint32_t)(mv - HALL_PAST_CLOSED_LO_MV) < HALL_PAST_CLOSED_WIDTH) {
        return DV_HALL_MID_LOW;
    }
    return DV_HALL_MID_HIGH;
}

// Caches for diagnostic logging without plumbing values through every caller.
static int s_hall_raw_last[DV_MOTOR_GROUP_COUNT];
static int s_hall_mv_last[DV_MOTOR_GROUP_COUNT];

static esp_err_t read_adc_mv(adc_channel_t channel, int *raw, int *mv)
{
    esp_err_t err = adc_oneshot_read(s_adc_handle, channel, raw);
    if (err != ESP_OK) return err;
    return adc_cali_raw_to_voltage(s_adc_cali, *raw, mv);
}

static dv_motor_hall_t read_hall(int g)
{
    int raw = 0;
    int mv = 0;
    esp_err_t err = read_adc_mv(DV_MOTOR_GROUPS[g].hall_adc_ch, &raw, &mv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hall read/convert grp=%d failed: %s", g, esp_err_to_name(err));
        return DV_HALL_INVALID;
    }
    s_hall_raw_last[g] = raw;
    s_hall_mv_last[g] = mv;
    return classify_hall_mv(mv);
}

// Read the config-detect ADC and classify to an active-group count. Returns
// -1 if the reading falls in a hysteresis gap — caller should hold the
// current config.
static int classify_hwconfig(void)
{
    int raw = 0;
    int mv = 0;
    if (read_adc_mv(DV_ADC_CONFIG_DETECT_CH, &raw, &mv) != ESP_OK) {
        return -1;
    }
    if ((uint32_t)(mv - DETECT_TWO_LO_MV) < DETECT_TWO_WIDTH_MV) return 4;
    if ((uint32_t)(mv - DETECT_ONE_LO_MV) < DETECT_ONE_WIDTH_MV) return 2;
    if (mv < DETECT_NONE_HI_MV) return 0;
    return -1;
}

// Cut both channels immediately (used for stop / init / dead-time entry).
static void hard_off_group(int g)
{
    ledc_set_duty(LEDC_MODE, DV_MOTOR_GROUPS[g].fwd_ledc_ch, 0);
    ledc_set_duty(LEDC_MODE, DV_MOTOR_GROUPS[g].rev_ledc_ch, 0);
    ledc_update_duty(LEDC_MODE, DV_MOTOR_GROUPS[g].fwd_ledc_ch);
    ledc_update_duty(LEDC_MODE, DV_MOTOR_GROUPS[g].rev_ledc_ch);
}

// Kick + fade the given channel from ~10% up to full duty. Caller is expected
// to have zeroed the opposite channel and observed the dead-time.
static void start_drive(int g, dir_t d)
{
    ledc_channel_t ch = chan_of(g, d);
    ledc_set_duty(LEDC_MODE, ch, DUTY_KICK);
    ledc_update_duty(LEDC_MODE, ch);
    ledc_set_fade_with_time(LEDC_MODE, ch, DUTY_MAX, FADE_UP_MS);
    ledc_fade_start(LEDC_MODE, ch, LEDC_FADE_NO_WAIT);
}

// Fade whichever channel is currently active down to 0, then hard-off both.
static void stop_drive(int g)
{
    group_state_t *st = &s_groups[g];
    if (st->dir != DIR_NONE) {
        ledc_channel_t ch = chan_of(g, st->dir);
        if (ledc_get_duty(LEDC_MODE, ch) != 0) {
            ledc_set_fade_with_time(LEDC_MODE, ch, 0, FADE_DOWN_MS);
            ledc_fade_start(LEDC_MODE, ch, LEDC_FADE_NO_WAIT);
            vTaskDelay(pdMS_TO_TICKS(FADE_DOWN_MS + 2));
        }
    }
    hard_off_group(g);
    st->dir = DIR_NONE;
    st->running = false;
}

// Enter drive state toward the target. Handles dead-time if reversing.
static void begin_drive_toward(int g, dv_motor_target_t target)
{
    group_state_t *st = &s_groups[g];
    dir_t new_dir = dir_for_target(target);
    if (new_dir == DIR_NONE) {
        stop_drive(g);
        return;
    }

    // Reversal: fully stop first + observe dead-time so we don't shoot through
    // the H-bridge or slam the mechanism.
    if (st->dir != DIR_NONE && st->dir != new_dir) {
        stop_drive(g);
        vTaskDelay(pdMS_TO_TICKS(DEAD_TIME_MS));
    } else if (st->dir == DIR_NONE) {
        // Cold start: still zero the opposite channel just in case.
        ledc_set_duty(LEDC_MODE, chan_of(g, new_dir == DIR_FWD ? DIR_REV : DIR_FWD), 0);
        ledc_update_duty(LEDC_MODE, chan_of(g, new_dir == DIR_FWD ? DIR_REV : DIR_FWD));
    }

    start_drive(g, new_dir);
    st->dir = new_dir;
    st->applied = target;
    st->running = true;
    st->drive_started_tick = xTaskGetTickCount();
}

// ---------- hot-plug reconfiguration ----------

// Configure LEDC channels for one motor group. All ADC channels are configured
// earlier, before LEDC is touched, to preserve stock's boot ordering.
static esp_err_t hw_init_group(int g)
{
    const dv_motor_group_t *m = &DV_MOTOR_GROUPS[g];
    ledc_channel_config_t fwd = {
        .gpio_num   = m->fwd_gpio,
        .speed_mode = LEDC_MODE,
        .channel    = m->fwd_ledc_ch,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config_t rev = fwd;
    rev.gpio_num = m->rev_gpio;
    rev.channel  = m->rev_ledc_ch;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&fwd), TAG, "ledc fwd grp=%d", g);
    ESP_RETURN_ON_ERROR(ledc_channel_config(&rev), TAG, "ledc rev grp=%d", g);

    return ESP_OK;
}

// Move to the new active count. Adds groups by configuring their peripherals;
// removes them by stopping the motor (channels are left configured but idle).
// Runs on the motor task, so no locking needed against tick_group.
static void reconfigure_to(int new_count)
{
    if (new_count == s_active_groups) return;

    ESP_LOGI(TAG, "hwconfig change: %d → %d motor groups",
             s_active_groups, new_count);

    if (new_count > s_active_groups) {
        for (int g = s_active_groups; g < new_count; ++g) {
            if (hw_init_group(g) != ESP_OK) {
                ESP_LOGE(TAG, "aborting reconfig: grp=%d init failed", g);
                return;
            }
            memset(&s_groups[g], 0, sizeof(s_groups[g]));
        }
    } else {
        for (int g = new_count; g < s_active_groups; ++g) {
            stop_drive(g);
            s_groups[g].target  = DV_MOTOR_TARGET_STOP;
            s_groups[g].applied = DV_MOTOR_TARGET_STOP;
        }
    }
    s_active_groups = new_count;
}

// Called from the task on a slow cadence. Debounces the ADC band across
// DETECT_DEBOUNCE_CYCLES consecutive samples before it changes anything.
static void tick_hwconfig(void)
{
    int band = classify_hwconfig();
    if (band < 0) {
        // Reading in a hysteresis gap — noise or partial connect; keep waiting.
        s_detect_streak = 0;
        return;
    }
    if (band == s_detect_last_band) {
        if (s_detect_streak < DETECT_DEBOUNCE_CYCLES) s_detect_streak++;
        if (s_detect_streak >= DETECT_DEBOUNCE_CYCLES && band != s_active_groups) {
            reconfigure_to(band);
        }
    } else {
        s_detect_last_band = band;
        s_detect_streak = 1;
    }
}

// ---------- state machine tick (per group) ----------

static void tick_group(int g)
{
    group_state_t *st = &s_groups[g];

    // Snapshot the API-visible target under lock.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dv_motor_target_t want = st->target;
    xSemaphoreGive(s_lock);

    dv_motor_hall_t hall = read_hall(g);
    st->hall_cached = hall;

    // Diagnostic: while a group is actively driving, log every sample so we
    // can see the whole trajectory (the 2026-07-10 diag caught a non-monotonic
    // hall response that a 1 Hz log would have missed).
    if (st->running) {
        ESP_LOGI(TAG, "grp=%d driving (want=%s) hall_raw=%d hall_mv=%d hall_state=%d",
                 g,
                 want == DV_MOTOR_TARGET_OPEN   ? "OPEN"
               : want == DV_MOTOR_TARGET_CLOSED ? "CLOSED" : "STOP",
                 s_hall_raw_last[g], s_hall_mv_last[g], (int)hall);
    }

    // Stop requested.
    if (want == DV_MOTOR_TARGET_STOP) {
        if (st->running) stop_drive(g);
        st->applied  = DV_MOTOR_TARGET_STOP;
        st->retries  = 0;
        st->arrived_consec = 0;
        st->gave_up  = false;
        return;
    }

    // Target changed since last drive → clear state and (re)start.
    if (st->applied != want) {
        st->retries        = 0;
        st->arrived_consec = 0;
        st->gave_up        = false;
        begin_drive_toward(g, want);
        return;
    }

    // Already at (or matched to) the target hall band. Debounce so a single
    // mid-travel spike doesn't stop us early.
    if (hall == hall_for_target(want)) {
        st->arrived_consec++;
        if (st->arrived_consec >= ARRIVED_DEBOUNCE_TICKS) {
            if (st->running) {
                ESP_LOGI(TAG, "grp=%d arrived (want=%s hall_raw=%d hall_mv=%d hall_state=%d)",
                         g,
                         want == DV_MOTOR_TARGET_OPEN ? "OPEN" : "CLOSED",
                         s_hall_raw_last[g], s_hall_mv_last[g], (int)hall);
                stop_drive(g);
            }
            st->retries = 0;
        }
        return;
    }
    // Not on target this sample — reset the debounce counter.
    st->arrived_consec = 0;

    // If we've given up on this target, stay stopped until dv_policy asks for
    // something different. Prevents the "retry forever after MAX_RETRIES" loop
    // that the field test caught (probably brownout root cause).
    if (st->gave_up) return;

    // Motor may have been stopped by give-up on the previous cycle. Kick it
    // back on if the target still wants motion.
    if (!st->running) {
        begin_drive_toward(g, want);
        return;
    }

    // Already driving; give the motor time to reach the target.
    TickType_t elapsed = xTaskGetTickCount() - st->drive_started_tick;
    if (elapsed < pdMS_TO_TICKS(VERIFY_TIMEOUT_MS)) return;

    // Timed out without hitting the endpoint — retry or give up.
    if (st->retries < MAX_RETRIES) {
        st->retries++;
        ESP_LOGW(TAG, "grp=%d stalled; retry %d/%d (hall_raw=%d hall_mv=%d hall_state=%d)",
                 g, st->retries, MAX_RETRIES,
                 s_hall_raw_last[g], s_hall_mv_last[g], (int)hall);
        stop_drive(g);
        vTaskDelay(pdMS_TO_TICKS(RETRY_PAUSE_MS));
        begin_drive_toward(g, want);
    } else {
        ESP_LOGE(TAG, "grp=%d gave up after %d retries (want=%s hall_raw=%d hall_mv=%d hall_state=%d)",
                 g, MAX_RETRIES,
                 want == DV_MOTOR_TARGET_OPEN   ? "OPEN"
               : want == DV_MOTOR_TARGET_CLOSED ? "CLOSED" : "STOP",
                 s_hall_raw_last[g], s_hall_mv_last[g], (int)hall);
        stop_drive(g);
        st->gave_up = true;
    }
}

static void motor_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int g = 0; g < s_active_groups; ++g) tick_group(g);

        // Sample the hardware-config ADC on a slower cadence.
        if (--s_detect_countdown <= 0) {
            s_detect_countdown = DETECT_INTERVAL_TICKS;
            tick_hwconfig();
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ---------- init ----------

static esp_err_t adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc_handle);
    if (err != ESP_OK) return err;

    adc_cali_line_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 0,
    };
    err = adc_cali_create_scheme_line_fitting(&cali, &s_adc_cali);
    if (err != ESP_OK) goto fail;

    adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    static const adc_channel_t stock_channel_order[] = {
        ADC_CHANNEL_2,
        ADC_CHANNEL_1,
        ADC_CHANNEL_0,
        ADC_CHANNEL_3,
        ADC_CHANNEL_7,
    };
    for (size_t i = 0; i < sizeof(stock_channel_order) / sizeof(stock_channel_order[0]); ++i) {
        err = adc_oneshot_config_channel(s_adc_handle, stock_channel_order[i], &channel);
        if (err != ESP_OK) goto fail;
    }
    ESP_LOGI(TAG, "ADC1 line fitting ready (12 dB, 12-bit)");
    return ESP_OK;

fail:
    if (s_adc_cali != NULL) {
        adc_cali_delete_scheme_line_fitting(s_adc_cali);
        s_adc_cali = NULL;
    }
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = NULL;
    return err;
}

esp_err_t dv_motor_init(void)
{
    if (s_task != NULL || s_adc_handle != NULL || s_adc_cali != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_groups, 0, sizeof(s_groups));
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    // Stock initializes ADC1 and line fitting before LEDC, RMT, WiFi, or MQTT
    // workers exist. app_main calls dv_motor_init first, so keeping ADC first
    // inside this function preserves that hardware ordering.
    esp_err_t err = adc_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        ESP_LOGE(TAG, "ADC1 calibration init failed: %s", esp_err_to_name(err));
        return err;
    }

    // LEDC timer + fade — needed even if no motors are currently connected,
    // so we're ready to bring channels online when a vent is plugged in.
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RES_BITS,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc_timer_config");
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "ledc_fade_func_install");

    // Initial synchronous detect: sample a few times to ride out startup noise.
    int initial = 0;
    for (int i = 0; i < 5; ++i) {
        int b = classify_hwconfig();
        if (b >= 0) initial = b;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int g = 0; g < initial; ++g) {
        ESP_RETURN_ON_ERROR(hw_init_group(g), TAG, "init grp=%d", g);
    }
    s_active_groups     = initial;
    s_detect_last_band  = initial;
    s_detect_streak     = DETECT_DEBOUNCE_CYCLES;
    s_detect_countdown  = DETECT_INTERVAL_TICKS;

    if (xTaskCreate(motor_task, "dv_motor", 4096, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "initialized; detected %d motor group(s) at boot", initial);
    return ESP_OK;
}

esp_err_t dv_motor_set_target(int group, dv_motor_target_t target)
{
    if (group < 0 || group >= s_active_groups) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_groups[group].target = target;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

dv_motor_hall_t dv_motor_hall(int group)
{
    if (group < 0 || group >= s_active_groups) return DV_HALL_INVALID;
    return s_groups[group].hall_cached;
}

bool dv_motor_is_running(int group)
{
    if (group < 0 || group >= s_active_groups) return false;
    return s_groups[group].running;
}

int dv_motor_active_groups(void) { return s_active_groups; }
