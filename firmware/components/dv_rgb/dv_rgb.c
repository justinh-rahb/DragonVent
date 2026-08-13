#include "dv_rgb.h"

#include "dc_lighting.h"
#include "dv_board.h"
#include "dv_motor.h"

#include "driver/spi_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <math.h>
#include <string.h>

static const char *TAG = "dv_rgb";

/* Physical output topology belongs to DragonVent. The strips use SPI+DMA: the
 * classic ESP32 RMT refill ISR can be starved and corrupt a WS2812 frame. */
#define LEDS_PER_STRIP 30
#define MAX_STRIPS 2
#define CFG_NVS_NS "app_nvs"
#define CFG_NVS_KEY "lighting"

static const gpio_num_t STRIP_GPIO[MAX_STRIPS] = { DV_PIN_RGB_STRIP_0, DV_PIN_RGB_STRIP_1 };
static int s_count;
static SemaphoreHandle_t s_lock;

static dv_lighting_t s_cfg = {
    .enabled = true, .brightness = 255,
    .open = {0, 0, 255}, .closed = {255, 0, 0}, .printing = {0, 255, 0},
    .use_printing = false, .use_temp = false, .temp_min_c = 25, .temp_max_c = 60,
    .effect = DV_FX_SOLID, .speed = 128,
    .error = {255, 0, 0}, .use_error = false,
    .mode = DV_LIGHT_MODE_VENT,
    .idle = {255, 255, 255}, .prep = {248, 163, 35}, .paused = {255, 255, 255}, .complete = {0, 255, 42},
};

static int s_target = DV_MOTOR_TARGET_CLOSED;
static int s_pstatus = DV_PS_NONE;
static bool s_printing, s_error;
static float s_bed = NAN;

static const uint8_t *printer_status_color(void)
{
    switch (s_pstatus) {
    case DV_PS_PREPARING: return s_cfg.prep;
    case DV_PS_PRINTING: return s_cfg.printing;
    case DV_PS_PAUSED: return s_cfg.paused;
    case DV_PS_COMPLETE: return s_cfg.complete;
    case DV_PS_ERROR: return s_cfg.error;
    default: return s_cfg.idle;
    }
}

static dc_rgb_t base_color(void)
{
    const uint8_t *base;
    uint8_t gradient[3];
    if (s_cfg.mode == DV_LIGHT_MODE_PRINTER) {
        base = printer_status_color();
    } else if (s_cfg.use_printing && s_printing) {
        base = s_cfg.printing;
    } else if (s_cfg.use_temp && !isnan(s_bed)) {
        int lo = s_cfg.temp_min_c, hi = s_cfg.temp_max_c;
        float factor = hi > lo ? (s_bed - lo) / (float)(hi - lo) : 0;
        if (factor < 0) factor = 0;
        if (factor > 1) factor = 1;
        for (int i = 0; i < 3; ++i)
            gradient[i] = (uint8_t)(s_cfg.open[i] + (int)((s_cfg.closed[i] - s_cfg.open[i]) * factor));
        base = gradient;
    } else {
        base = s_target == DV_MOTOR_TARGET_OPEN ? s_cfg.open : s_cfg.closed;
    }
    return (dc_rgb_t){base[0], base[1], base[2]};
}

static dc_lighting_effect_t core_effect(uint8_t effect)
{
    switch (effect) {
    case DV_FX_CYCLE: return DC_LIGHTING_CYCLE;
    case DV_FX_RAINBOW: return DC_LIGHTING_RAINBOW;
    case DV_FX_BREATHE: return DC_LIGHTING_BREATHE;
    case DV_FX_STROBE: return DC_LIGHTING_STROBE;
    case DV_FX_WAVE: return DC_LIGHTING_WAVE;
    case DV_FX_MARQUEE: return DC_LIGHTING_MARQUEE;
    case DV_FX_CYLON: return DC_LIGHTING_CYLON;
    default: return DC_LIGHTING_SOLID;
    }
}

/* Caller holds s_lock. Product policy resolves state and error precedence;
 * dc_lighting only renders the selected colour/effect over the declared layout. */
static void apply_locked(void)
{
    if (!s_cfg.enabled) { (void)dc_lighting_off(); return; }
    if (s_cfg.use_error && s_error) {
        (void)dc_lighting_set((dc_rgb_t){s_cfg.error[0], s_cfg.error[1], s_cfg.error[2]}, DC_LIGHTING_STROBE, 64);
        return;
    }
    dc_rgb_t color = base_color();
    /* Cylon is the family Easter egg, deliberately independent of the current
     * vent/printer palette. The shared renderer itself remains colour-neutral. */
    if (s_cfg.effect == DV_FX_CYLON) color = (dc_rgb_t){255, 0, 0};
    (void)dc_lighting_set(color, core_effect(s_cfg.effect), s_cfg.speed);
}

void dv_rgb_get_config(dv_lighting_t *out)
{
    if (!out) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_lock) xSemaphoreGive(s_lock);
}

esp_err_t dv_rgb_set_config(const dv_lighting_t *cfg)
{
    if (!cfg || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    if (s_cfg.effect > DV_FX_CYLON) s_cfg.effect = DV_FX_SOLID;
    (void)dc_lighting_set_brightness(s_cfg.brightness);
    for (int i = 0; i < s_count; ++i) (void)dc_lighting_set_output_reverse(i, s_cfg.reverse);
    apply_locked();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, CFG_NVS_KEY, &s_cfg, sizeof(s_cfg));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    xSemaphoreGive(s_lock);
    return err;
}

static void cfg_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_blob(nvs, CFG_NVS_KEY, NULL, &size) == ESP_OK && size > 0 && size <= sizeof(s_cfg)) {
        uint8_t saved[sizeof(s_cfg)];
        size_t capacity = sizeof(saved);
        if (nvs_get_blob(nvs, CFG_NVS_KEY, saved, &capacity) == ESP_OK) {
            memcpy(&s_cfg, saved, capacity);
            ESP_LOGI(TAG, "loaded lighting config (%u B) from NVS", (unsigned)capacity);
        }
    }
    nvs_close(nvs);
}

int dv_rgb_strip_count(void) { return s_count; }

esp_err_t dv_rgb_start(void)
{
    if (s_lock) return ESP_ERR_INVALID_STATE;
    int groups = dv_motor_active_groups();
    int strips = groups >= 2 ? 2 : 0;
    if (strips == 0) {
        ESP_LOGI(TAG, "no RGB strips (config-detect reports %d motor groups)", groups);
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    cfg_load();

    dc_lighting_output_t outputs[MAX_STRIPS] = {0};
    for (int i = 0; i < strips; ++i) {
        outputs[i] = (dc_lighting_output_t){
            .gpio = STRIP_GPIO[i], .pixels = LEDS_PER_STRIP, .reverse = s_cfg.reverse,
            .transport = DC_LIGHTING_TRANSPORT_SPI_DMA,
            .spi_host = i == 0 ? SPI2_HOST : SPI3_HOST,
        };
    }
    esp_err_t err = dc_lighting_start(&(dc_lighting_config_t){
        .outputs = outputs, .output_count = strips, .brightness = s_cfg.brightness,
        .fps = 33, .layout = DC_LIGHTING_LAYOUT_PER_OUTPUT,
    });
    if (err != ESP_OK) return err;
    s_count = strips;
    xSemaphoreTake(s_lock, portMAX_DELAY); apply_locked(); xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "started %d SPI+DMA WS2812 output(s), %d LEDs each", s_count, LEDS_PER_STRIP);
    return ESP_OK;
}

void dv_rgb_update(int target, int status, float bed_temp_c)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target = target;
    s_pstatus = status;
    s_printing = status == DV_PS_PRINTING;
    s_error = status == DV_PS_ERROR;
    s_bed = bed_temp_c;
    apply_locked();
    xSemaphoreGive(s_lock);
}
