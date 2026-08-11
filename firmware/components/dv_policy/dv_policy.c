#include "dv_policy.h"
#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_moonraker.h"
#include "dc_source.h"
#include "dv_motor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "dv_policy";

#define NVS_NS         "app_nvs"
#define KEY_MODE       "policy_mode"
#define KEY_BED_OPEN   "bed_open_c"
#define KEY_BED_CLOSE  "bed_close_c"
#define KEY_MAN_TGT    "man_tgt"
#define KEY_FILAMENT   "fil_rules"

// Defaults for the bed-temperature hysteresis if the user hasn't changed
// them: open the vent when bed climbs above BED_OPEN_C_DEFAULT, close it
// when it drops below BED_CLOSE_C_DEFAULT, hold current state between.
// Chosen so residual heat after a print keeps the vent open until the
// chamber cools.
#define BED_OPEN_C_DEFAULT   45.0f
#define BED_CLOSE_C_DEFAULT  35.0f
#define TICK_MS      1000

// Material-aware behavior. When the printer publishes a known material name,
// PLA/PETG/etc. prefer venting for cooling; ABS/ASA/PC prefer sealing for
// heat retention. If the material is unknown, we fall back to the temperature
// hysteresis above.
//
// Bambu's stock firmware brags about this on the product page but doesn't
// document the exact rules — these defaults are conservative and match what
// most enclosure users do by hand.
typedef enum {
    MAT_PREFER_UNKNOWN,   // no rule; use temperature hysteresis
    MAT_PREFER_OPEN,      // cool-running plastics (PLA, PETG, TPU)
    MAT_PREFER_SEALED,    // hot-chamber plastics (ABS, ASA, PC, PA)
} material_pref_t;

// Default filament rules — the plastics most enclosure users seal vs. vent by
// hand. User-editable + persisted (KEY_FILAMENT).
static const dv_filament_rule_t DEFAULT_FILAMENT_RULES[] = {
    {"PLA",  false}, {"PETG", false}, {"PET",  false}, {"TPU",  false},
    {"ABS",  true},  {"ASA",  true},  {"PC",   true},  {"PA",   true},  {"HIPS", true},
};
static dv_filament_rule_t s_rules[DV_FILAMENT_MAX];
static int                s_rule_count = 0;

// First rule whose (uppercase) name is a prefix of the filament wins.
static material_pref_t material_preference(const char *m)
{
    if (m == NULL || m[0] == '\0') return MAT_PREFER_UNKNOWN;
    char up[16];
    size_t k = 0;
    for (const char *p = m; *p && k < sizeof(up) - 1; ++p) up[k++] = (char)toupper((unsigned char)*p);
    up[k] = '\0';
    for (int i = 0; i < s_rule_count; ++i) {
        size_t n = strnlen(s_rules[i].name, sizeof(s_rules[i].name));
        if (n > 0 && strncmp(up, s_rules[i].name, n) == 0) {
            return s_rules[i].seal ? MAT_PREFER_SEALED : MAT_PREFER_OPEN;
        }
    }
    return MAT_PREFER_UNKNOWN;
}

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static dv_policy_mode_t  s_mode = DV_POLICY_MODE_AUTO;
static dv_motor_target_t s_manual_target  = DV_MOTOR_TARGET_CLOSED;
static dv_motor_target_t s_current_target = DV_MOTOR_TARGET_CLOSED;
static float             s_bed_open_c  = BED_OPEN_C_DEFAULT;
static float             s_bed_close_c = BED_CLOSE_C_DEFAULT;
static dc_ctl_source_t   s_source = DC_SRC_KLIPPER;

typedef struct {
    bool reliable;
    bool error;
    bool active;
    float bed_temp;
    char material[16];
    const char *state;
} auto_input_t;

static void read_auto_input(auto_input_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = "unknown";

    if (s_source == DC_SRC_KLIPPER) {
        dc_moonraker_status_t st = {0};
        if (dc_moonraker_get_status(&st) != ESP_OK) return;
        out->reliable = st.state == DC_MK_SUBSCRIBED &&
                        st.printer != DC_PRINTER_UNKNOWN;
        out->error = st.printer == DC_PRINTER_ERROR;
        out->active = st.printer == DC_PRINTER_PRINTING ||
                      st.printer == DC_PRINTER_PREPARING ||
                      st.printer == DC_PRINTER_PAUSED;
        out->bed_temp = st.bed_temp;
        snprintf(out->material, sizeof(out->material), "%s", st.material);
        out->state = dc_printer_state_str(st.printer);
        return;
    }

    if (s_source == DC_SRC_BAMBU) {
        dc_bambu_status_t st = {0};
        if (dc_bambu_get_status(&st) != ESP_OK) return;
        out->reliable = st.state == DC_BAMBU_SUBSCRIBED;
        out->active = st.printing;
        out->bed_temp = st.bed_temp;
        snprintf(out->material, sizeof(out->material), "%s", st.filament);
        out->state = st.printing ? "printing" : "idle";
        return;
    }

    out->state = dc_source_str(s_source);
}

static void apply_target(dv_motor_target_t t)
{
    int n = dv_motor_active_groups();
    for (int g = 0; g < n; ++g) dv_motor_set_target(g, t);
    s_current_target = t;
}

// AUTO decision. Returns the target we should be driving toward, given the
// current Moonraker snapshot. If we don't have reliable data, keep whatever
// we're already commanding.
//
// Order of consideration:
//   1. No subscription yet / unknown state -> hold
//   2. ERROR                               -> hold (don't move on a broken printer)
//   3. Printing/preparing/paused + material rule wants sealed -> CLOSED
//   4. Printing/preparing/paused           -> OPEN
//   5. Idle/complete, bed still hot        -> OPEN  (residual heat)
//   6. Idle/complete, bed cool             -> CLOSED
//   7. Otherwise                           -> hold  (hysteresis band)
static dv_motor_target_t decide_auto_target(const auto_input_t *st)
{
    if (!st->reliable || st->error) return s_current_target;

    if (st->active) {
        material_pref_t mat = material_preference(st->material);
        if (mat == MAT_PREFER_SEALED) return DV_MOTOR_TARGET_CLOSED;
        // MAT_PREFER_OPEN and MAT_PREFER_UNKNOWN both open during a print —
        // the tester's ask is that PLA-style vents while ABS seals; when we
        // don't know the material we default to venting, which is the safer
        // choice for PLA-family plastics that dominate hobby printing.
        return DV_MOTOR_TARGET_OPEN;
    }

    // Idle / complete: use bed-temp hysteresis so residual chamber heat
    // keeps the vent open until things cool down.
    if (st->bed_temp > s_bed_open_c)  return DV_MOTOR_TARGET_OPEN;
    if (st->bed_temp < s_bed_close_c) return DV_MOTOR_TARGET_CLOSED;
    return s_current_target;
}

static void policy_task(void *arg)
{
    (void)arg;
    for (;;) {
        auto_input_t st;
        read_auto_input(&st);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        dv_motor_target_t want = (s_mode == DV_POLICY_MODE_MANUAL)
                                     ? s_manual_target
                                     : decide_auto_target(&st);
        if (want != s_current_target) {
            const char *target_str = (want == DV_MOTOR_TARGET_OPEN)   ? "OPEN"
                                   : (want == DV_MOTOR_TARGET_CLOSED) ? "CLOSED"
                                                                     : "STOP";
            ESP_LOGI(TAG, "target -> %s (mode=%s, printer=%s, mat=%s, bed=%.1f)",
                     target_str,
                     s_mode == DV_POLICY_MODE_AUTO ? "AUTO" : "MANUAL",
                     st.state,
                     st.material[0] ? st.material : "?",
                     st.bed_temp);
            dc_evlog_add("vent %s (%s, printer=%s, mat=%s, bed=%.1fC)",
                         target_str,
                         s_mode == DV_POLICY_MODE_AUTO ? "AUTO" : "MANUAL",
                         st.state,
                         st.material[0] ? st.material : "?",
                         st.bed_temp);
            apply_target(want);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ---------- NVS ----------

// NVS stores °C values as centi-degrees in a u32 so we don't have to teach
// nvs about floats. 45.0 °C -> 4500. Range is clamped in setter.
static float centi_to_c(uint32_t v) { return (float)v / 100.0f; }
static uint32_t c_to_centi(float c)
{
    if (c < 0.0f)   c = 0.0f;
    if (c > 200.0f) c = 200.0f;
    return (uint32_t)(c * 100.0f + 0.5f);
}

static void load_persisted(void)
{
    // Filament-rule defaults up front, so they apply even if NVS can't be opened.
    memcpy(s_rules, DEFAULT_FILAMENT_RULES, sizeof(DEFAULT_FILAMENT_RULES));
    s_rule_count = sizeof(DEFAULT_FILAMENT_RULES) / sizeof(DEFAULT_FILAMENT_RULES[0]);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t m = 0;
    if (nvs_get_u8(h, KEY_MODE, &m) == ESP_OK) {
        s_mode = (m == DV_POLICY_MODE_MANUAL) ? DV_POLICY_MODE_MANUAL
                                              : DV_POLICY_MODE_AUTO;
    }
    uint8_t t = 0;
    if (nvs_get_u8(h, KEY_MAN_TGT, &t) == ESP_OK) {
        // Only OPEN and CLOSED are legal persisted values; anything else
        // (STOP, garbage) falls back to CLOSED so a stale NVS can't jam a
        // half-driven target on boot.
        s_manual_target = (t == DV_MOTOR_TARGET_OPEN) ? DV_MOTOR_TARGET_OPEN
                                                     : DV_MOTOR_TARGET_CLOSED;
    }
    uint32_t v = 0;
    if (nvs_get_u32(h, KEY_BED_OPEN,  &v) == ESP_OK) s_bed_open_c  = centi_to_c(v);
    if (nvs_get_u32(h, KEY_BED_CLOSE, &v) == ESP_OK) s_bed_close_c = centi_to_c(v);

    // Filament rules: override the defaults from the NVS blob if present + valid
    // (a whole number of rules). On any error nvs_get_blob leaves s_rules alone.
    size_t rsz = sizeof(s_rules);
    if (nvs_get_blob(h, KEY_FILAMENT, s_rules, &rsz) == ESP_OK &&
        rsz >= sizeof(dv_filament_rule_t) && rsz % sizeof(dv_filament_rule_t) == 0) {
        s_rule_count = (int)(rsz / sizeof(dv_filament_rule_t));
    }
    nvs_close(h);
}

static void save_mode(dv_policy_mode_t m)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_MODE, (uint8_t)m);
    nvs_commit(h);
    nvs_close(h);
}

static void save_manual_target(dv_motor_target_t t)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_MAN_TGT, (uint8_t)t);
    nvs_commit(h);
    nvs_close(h);
}

static void save_thresholds(float open_c, float close_c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, KEY_BED_OPEN,  c_to_centi(open_c));
    nvs_set_u32(h, KEY_BED_CLOSE, c_to_centi(close_c));
    nvs_commit(h);
    nvs_close(h);
}

int dv_policy_filament_rules(dv_filament_rule_t *out, int max)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_rule_count;
    if (n > max) n = max;
    if (out && n > 0) memcpy(out, s_rules, (size_t)n * sizeof(dv_filament_rule_t));
    if (s_lock) xSemaphoreGive(s_lock);
    return n;
}

esp_err_t dv_policy_set_filament_rules(const dv_filament_rule_t *rules, int count)
{
    if (count < 0 || count > DV_FILAMENT_MAX || (count > 0 && rules == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_rules, 0, sizeof(s_rules));
    for (int i = 0; i < count; ++i) {
        size_t k = 0;   // store names uppercase for case-insensitive matching
        for (const char *p = rules[i].name; *p && k < sizeof(s_rules[i].name) - 1; ++p) {
            s_rules[i].name[k++] = (char)toupper((unsigned char)*p);
        }
        s_rules[i].name[k] = '\0';
        s_rules[i].seal = rules[i].seal;
    }
    s_rule_count = count;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (count == 0) nvs_erase_key(h, KEY_FILAMENT);
        else nvs_set_blob(h, KEY_FILAMENT, s_rules, (size_t)count * sizeof(dv_filament_rule_t));
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

// ---------- public API ----------

esp_err_t dv_policy_start(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    load_persisted();
    s_source = dc_source_get();
    if (xTaskCreate(policy_task, "dv_policy", 4096, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started in %s mode", s_mode == DV_POLICY_MODE_AUTO ? "AUTO" : "MANUAL");
    return ESP_OK;
}

esp_err_t dv_policy_set_mode(dv_policy_mode_t mode)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = mode;
    save_mode(mode);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

dv_policy_mode_t dv_policy_get_mode(void) { return s_mode; }

esp_err_t dv_policy_set_manual_target(dv_motor_target_t t)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_manual_target = t;
    save_manual_target(t);
    // If we're already in manual mode, apply immediately instead of waiting
    // for the next tick.
    if (s_mode == DV_POLICY_MODE_MANUAL && t != s_current_target) apply_target(t);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

dv_motor_target_t dv_policy_get_target(void) { return s_current_target; }

esp_err_t dv_policy_get_thresholds(float *bed_open_c, float *bed_close_c)
{
    if (bed_open_c == NULL || bed_close_c == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *bed_open_c  = s_bed_open_c;
    *bed_close_c = s_bed_close_c;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dv_policy_set_thresholds(float bed_open_c, float bed_close_c)
{
    // OPEN must be strictly above CLOSE, otherwise the hysteresis band
    // collapses / inverts and the vent will flap.
    if (!(bed_open_c > bed_close_c)) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bed_open_c  = bed_open_c;
    s_bed_close_c = bed_close_c;
    save_thresholds(bed_open_c, bed_close_c);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dv_policy_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_MODE);
    nvs_erase_key(h, KEY_MAN_TGT);
    nvs_erase_key(h, KEY_BED_OPEN);
    nvs_erase_key(h, KEY_BED_CLOSE);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
