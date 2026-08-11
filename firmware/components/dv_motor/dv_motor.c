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
#include "nvs.h"

#include <stdbool.h>
#include <stdlib.h>
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
#define VERIFY_TIMEOUT_MS   1200                // seat must confirm within this window
                                                // (vent travel is ~400-540 ms; the old
                                                // 200 ms fired mid-travel and forced a retry)
#define RETRY_PAUSE_MS      50                  // brief stop between retries
#define MAX_RETRIES         4
#define TICK_MS             10                  // task loop period

// Per-unit hall endstop calibration.
//
// Stock (and our earlier port) used FIXED calibrated-mV bands — OPEN 640-960,
// CLOSED 1360-1680. On-hardware that turned out to be a per-board gamble: this
// unit's ADC/hall gain reads ~20-25% low, so its real endstops sit at ~590 mV
// (open) and ~1175 mV (closed) — squarely in the *gaps* of the fixed bands, so
// the classifier never confirmed and the motor re-drove MAX_RETRIES times (the
// audible "buzz") even though the vent had physically seated. Rather than bake
// in one board's numbers, we LEARN each unit's endstop levels at runtime and
// classify against them.
//
// The learning signal is the hard stop itself: when the vent seats, the hall
// reading goes dead-stable (settles). We detect that settle, record the level
// for the direction we were driving, and persist it. No magic thresholds; works
// on any unit regardless of ADC gain or hall polarity.
#define SETTLE_DELTA_MV        45      // samples within this of each other = stable
#define SETTLE_TICKS           6       // ~60 ms of continuous stability = seated
#define MOVED_DELTA_MV         200     // hall moved this far from start = in motion
#define STUCK_SETTLE_MS        350     // stable-from-start this long = already seated
#define MIN_SEPARATION_MV      300     // learned OPEN/CLOSED must differ by >= this
#define LEARN_WRITE_DELTA_MV   20      // only persist when a level shifts > this
#define CLASSIFY_MARGIN_MV     250     // in-band half-width around a learned level
#define ARRIVED_DEBOUNCE_TICKS 3       // 30 ms of continuous in-band samples

// Persisted per-group endstop levels. -1 = not yet learned.
#define CAL_NVS_NS   "app_nvs"
#define CAL_NVS_KEY  "hall_cal"
typedef struct {
    int16_t open_mv;
    int16_t closed_mv;
} hall_cal_t;
static hall_cal_t s_cal[DV_MOTOR_GROUP_COUNT];

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
    int               arrived_consec;      // consecutive ticks reading target (calibrated fast-path)
    bool              arrived;             // reached target and holding; don't re-drive
    bool              gave_up;             // exhausted retries; wait for new target
    TickType_t        drive_started_tick;
    dv_motor_hall_t   hall_cached;
    // Settle detector — drive-scoped, reset at the start of every drive.
    int               drive_start_mv;      // hall mv when this drive began
    int               settle_ref;          // reference mv for the stability window
    int               settle_count;        // consecutive samples within SETTLE_DELTA_MV
    bool              moved;               // hall has left the start region this drive
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

// ---------- per-unit hall calibration ----------

static void cal_load(void)
{
    for (int g = 0; g < DV_MOTOR_GROUP_COUNT; ++g) {
        s_cal[g].open_mv   = -1;
        s_cal[g].closed_mv = -1;
    }
    nvs_handle_t h;
    if (nvs_open(CAL_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_cal);
    nvs_get_blob(h, CAL_NVS_KEY, s_cal, &sz);   // leaves defaults on any error
    nvs_close(h);
}

static void cal_save(void)
{
    nvs_handle_t h;
    if (nvs_open(CAL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, CAL_NVS_KEY, s_cal, sizeof(s_cal));
    nvs_commit(h);
    nvs_close(h);
}

// Record a freshly-seated endstop level for one group/direction. Rejects
// implausible values (too close to the opposite endstop, e.g. a mid-travel jam)
// and skips the flash write when the level hasn't meaningfully changed.
static void learn_level(int g, dv_motor_target_t dir, int mv)
{
    if (mv <= 0) return;
    int16_t *slot  = (dir == DV_MOTOR_TARGET_OPEN) ? &s_cal[g].open_mv : &s_cal[g].closed_mv;
    int16_t  other = (dir == DV_MOTOR_TARGET_OPEN) ? s_cal[g].closed_mv : s_cal[g].open_mv;
    if (other >= 0 && abs(mv - other) < MIN_SEPARATION_MV) return;
    if (*slot >= 0 && abs(mv - *slot) <= LEARN_WRITE_DELTA_MV) return;
    *slot = (int16_t)mv;
    cal_save();
    ESP_LOGI(TAG, "grp=%d learned %s endstop = %d mV",
             g, dir == DV_MOTOR_TARGET_OPEN ? "OPEN" : "CLOSED", mv);
}

// Classify a reading against this group's LEARNED endstop levels. Until a level
// is learned it reads MID_HIGH (in transit), which is correct — arrival is then
// confirmed purely by settle detection, which is also what learns the level.
static dv_motor_hall_t classify_hall_mv(int g, int mv)
{
    if (mv == 0) return DV_HALL_INVALID;
    int o = s_cal[g].open_mv, c = s_cal[g].closed_mv;
    int margin = CLASSIFY_MARGIN_MV;
    if (o >= 0 && c >= 0) {
        int half = abs(o - c) / 2 - 20;   // keep the two bands from overlapping
        if (half > 0 && half < margin) margin = half;
    }
    if (o >= 0 && abs(mv - o) <= margin) return DV_HALL_OPEN;
    if (c >= 0 && abs(mv - c) <= margin) return DV_HALL_CLOSED;
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
    return classify_hall_mv(g, mv);
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
    // Reset the settle detector against the position we're starting from.
    st->drive_start_mv = s_hall_mv_last[g];
    st->settle_ref     = s_hall_mv_last[g];
    st->settle_count   = 0;
    st->moved          = false;
    st->arrived_consec = 0;
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

// Advance the settle detector with one sample. Flags "moved" once the reading
// leaves the region this drive started in, and counts consecutive stable samples.
static void settle_update(int g, int mv)
{
    group_state_t *st = &s_groups[g];
    if (abs(mv - st->settle_ref) <= SETTLE_DELTA_MV) {
        if (st->settle_count < 100000) st->settle_count++;
    } else {
        st->settle_ref   = mv;
        st->settle_count = 0;
    }
    if (abs(mv - st->drive_start_mv) > MOVED_DELTA_MV) st->moved = true;
}

// Seated == reading stable for SETTLE_TICKS AND either it traveled to get here
// (moved) or it has been stable from the very start long enough that it must
// already be against this endstop.
static bool group_seated(int g, TickType_t elapsed)
{
    group_state_t *st = &s_groups[g];
    if (st->settle_count < SETTLE_TICKS) return false;
    if (st->moved) return true;
    return elapsed >= pdMS_TO_TICKS(STUCK_SETTLE_MS);
}

static void tick_group(int g)
{
    group_state_t *st = &s_groups[g];

    // Snapshot the API-visible target under lock.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dv_motor_target_t want = st->target;
    xSemaphoreGive(s_lock);

    dv_motor_hall_t hall = read_hall(g);
    st->hall_cached = hall;
    int mv = s_hall_mv_last[g];

    // Stop requested.
    if (want == DV_MOTOR_TARGET_STOP) {
        if (st->running) stop_drive(g);
        st->applied  = DV_MOTOR_TARGET_STOP;
        st->retries  = 0;
        st->arrived_consec = 0;
        st->arrived  = false;
        st->gave_up  = false;
        return;
    }

    // Target changed since last drive → clear state and (re)start.
    if (st->applied != want) {
        st->retries = 0;
        st->gave_up = false;
        st->arrived = false;
        begin_drive_toward(g, want);   // also resets the settle detector
        return;
    }

    // Reached the target and holding — stay stopped until dv_policy commands a
    // different target. Without this latch we'd re-drive every tick (arrive →
    // stop → !running → re-drive), churning the LEDC fades until the interrupt
    // watchdog fires. Cleared above when the target changes.
    if (st->arrived) return;

    // Given up on this target: stay stopped until dv_policy asks for something
    // else. Prevents the "retry forever after MAX_RETRIES" loop the field test
    // caught (probable brownout root cause).
    if (st->gave_up) return;

    // Motor may have been stopped by a give-up last cycle; kick it back on.
    if (!st->running) {
        begin_drive_toward(g, want);
        return;
    }

    settle_update(g, mv);
    TickType_t elapsed = xTaskGetTickCount() - st->drive_started_tick;

    // Diagnostic: log every sample while driving so we can see the whole
    // trajectory (a 2026-07-10 diag caught a non-monotonic hall response a 1 Hz
    // log would miss). settle/moved make the seat decision auditable.
    ESP_LOGI(TAG, "grp=%d driving (want=%s) hall_raw=%d hall_mv=%d hall_state=%d settle=%d moved=%d",
             g, want == DV_MOTOR_TARGET_OPEN ? "OPEN" : "CLOSED",
             s_hall_raw_last[g], mv, (int)hall, st->settle_count, (int)st->moved);

    // Fast path on a calibrated unit: steadily reading the target's learned band
    // is an arrival even before the mechanical settle window fills.
    bool in_band = (hall == hall_for_target(want));
    if (in_band) {
        if (++st->arrived_consec < ARRIVED_DEBOUNCE_TICKS) return;
    } else {
        st->arrived_consec = 0;
    }

    // Primary path: the vent seated (hall went dead-stable at the hard stop).
    if (in_band || group_seated(g, elapsed)) {
        learn_level(g, want, mv);   // record this unit's endstop level, persist
        ESP_LOGI(TAG, "grp=%d arrived (want=%s hall_mv=%d) in %d ms",
                 g, want == DV_MOTOR_TARGET_OPEN ? "OPEN" : "CLOSED",
                 mv, (int)(elapsed * portTICK_PERIOD_MS));
        stop_drive(g);
        st->arrived = true;
        st->retries = 0;
        return;
    }

    // Not seated yet — give the drive its full window before retrying.
    if (elapsed < pdMS_TO_TICKS(VERIFY_TIMEOUT_MS)) return;

    // Timed out without seating — retry or give up (safety only; a healthy vent
    // seats and settles well within the window).
    if (st->retries < MAX_RETRIES) {
        st->retries++;
        ESP_LOGW(TAG, "grp=%d stalled; retry %d/%d (hall_mv=%d hall_state=%d)",
                 g, st->retries, MAX_RETRIES, mv, (int)hall);
        stop_drive(g);
        vTaskDelay(pdMS_TO_TICKS(RETRY_PAUSE_MS));
        begin_drive_toward(g, want);
    } else {
        ESP_LOGE(TAG, "grp=%d gave up after %d retries (want=%s hall_mv=%d hall_state=%d)",
                 g, MAX_RETRIES,
                 want == DV_MOTOR_TARGET_OPEN ? "OPEN" : "CLOSED", mv, (int)hall);
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
    cal_load();
    for (int g = 0; g < DV_MOTOR_GROUP_COUNT; ++g) {
        if (s_cal[g].open_mv >= 0 || s_cal[g].closed_mv >= 0) {
            ESP_LOGI(TAG, "grp=%d hall cal: open=%d closed=%d mV",
                     g, s_cal[g].open_mv, s_cal[g].closed_mv);
        }
    }
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
