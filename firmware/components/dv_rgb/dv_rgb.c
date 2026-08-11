#include "dv_rgb.h"
#include "dv_board.h"
#include "dv_motor.h"

#include "led_strip.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <math.h>
#include <string.h>

static const char *TAG = "dv_rgb";

// Physical LED count varies (RE saw 16 on one board, 27 across two). Driving
// MORE than exist is harmless with a solid fill — surplus refresh words fall off
// the end of the chain — so use a generous per-strip count and drive both GPIO
// outputs. An unpopulated strip's GPIO just clocks out to nothing.
#define LEDS_PER_STRIP  30
#define MAX_STRIPS      2

#define CFG_NVS_NS   "app_nvs"
#define CFG_NVS_KEY  "lighting"

static const gpio_num_t STRIP_GPIO[MAX_STRIPS] = {
    DV_PIN_RGB_STRIP_0,   // GPIO 14
    DV_PIN_RGB_STRIP_1,   // GPIO 4 (2-vent kit only)
};

static led_strip_handle_t s_strips[MAX_STRIPS];
static int                s_count = 0;
static SemaphoreHandle_t  s_lock  = NULL;

// Lighting config (defaults keep the validated blue/red behavior; the printing
// and temp layers are opt-in so nothing changes until the user enables them).
static dv_lighting_t s_cfg = {
    .enabled = true, .brightness = 255,
    .open    = {0, 0, 255},   // blue = cool
    .closed  = {255, 0, 0},   // red = hot
    .printing = {0, 255, 0},  // green = active
    .use_printing = false, .use_temp = false,
    .temp_min_c = 25, .temp_max_c = 60,
};

// Latest state fed to the policy, and the last color actually pushed (so we skip
// redundant RMT refreshes).
static int     s_target   = DV_MOTOR_TARGET_CLOSED;
static bool    s_printing = false;
static float   s_bed      = NAN;
static uint8_t s_last[3]  = {1, 1, 1};   // impossible first value forces a push

static esp_err_t make_strip(gpio_num_t gpio, led_strip_handle_t *out)
{
    led_strip_config_t strip = {
        .strip_gpio_num   = gpio,
        .max_leds         = LEDS_PER_STRIP,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags            = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,   // 10 MHz, standard WS2812 timing
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    return led_strip_new_rmt_device(&strip, &rmt, out);
}

// Push one solid color to every LED on every strip (assumes s_lock held).
static void fill_locked(const uint8_t rgb[3])
{
    for (int i = 0; i < s_count; ++i) {
        for (int j = 0; j < LEDS_PER_STRIP; ++j) {
            led_strip_set_pixel(s_strips[i], j, rgb[0], rgb[1], rgb[2]);
        }
        led_strip_refresh(s_strips[i]);
    }
}

// Resolve the current lighting policy to a final RGB (brightness applied).
static void compute(uint8_t out[3])
{
    if (!s_cfg.enabled) { out[0] = out[1] = out[2] = 0; return; }

    const uint8_t *base;
    uint8_t grad[3];
    if (s_cfg.use_printing && s_printing) {
        base = s_cfg.printing;
    } else if (s_cfg.use_temp && !isnan(s_bed)) {
        int lo = s_cfg.temp_min_c, hi = s_cfg.temp_max_c;
        float f = (hi > lo) ? (s_bed - lo) / (float)(hi - lo) : 0.0f;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        for (int k = 0; k < 3; ++k) {
            grad[k] = (uint8_t)(s_cfg.open[k] + (int)((s_cfg.closed[k] - s_cfg.open[k]) * f));
        }
        base = grad;
    } else {
        base = (s_target == DV_MOTOR_TARGET_OPEN) ? s_cfg.open : s_cfg.closed;
    }
    for (int k = 0; k < 3; ++k) {
        out[k] = (uint8_t)((uint16_t)base[k] * s_cfg.brightness / 255);
    }
}

static void apply_locked(void)
{
    uint8_t rgb[3];
    compute(rgb);
    if (memcmp(rgb, s_last, 3) != 0) {
        fill_locked(rgb);
        memcpy(s_last, rgb, 3);
    }
}

void dv_rgb_update(int target, bool printing, float bed_temp_c)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target = target;
    s_printing = printing;
    s_bed = bed_temp_c;
    apply_locked();
    xSemaphoreGive(s_lock);
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
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, CFG_NVS_KEY, &s_cfg, sizeof(s_cfg));
        nvs_commit(h);
        nvs_close(h);
    }
    apply_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    dv_lighting_t stored;
    size_t sz = sizeof(stored);
    if (nvs_get_blob(h, CFG_NVS_KEY, &stored, &sz) == ESP_OK && sz == sizeof(stored)) {
        s_cfg = stored;
        ESP_LOGI(TAG, "loaded lighting config from NVS");
    }
    nvs_close(h);
}

int dv_rgb_strip_count(void) { return s_count; }

esp_err_t dv_rgb_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;

    int groups = dv_motor_active_groups();
    // The RE mapped 2 groups -> 1 strip, but real single-vent boards drive both
    // GPIO 14 and GPIO 4 (an unpopulated one is a no-op), so bring up both
    // whenever any vent is connected.
    int strips = (groups >= 2) ? 2 : 0;
    if (strips == 0) {
        ESP_LOGI(TAG, "no RGB strips (config-detect reports %d motor groups)", groups);
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    cfg_load();

    for (int i = 0; i < strips; ++i) {
        esp_err_t err = make_strip(STRIP_GPIO[i], &s_strips[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "strip %d (GPIO %d) init failed: %s",
                     i, STRIP_GPIO[i], esp_err_to_name(err));
            return err;
        }
        led_strip_clear(s_strips[i]);
        ++s_count;
    }
    ESP_LOGI(TAG, "initialized %d WS2812 strip(s), %d LEDs each", s_count, LEDS_PER_STRIP);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_locked();   // initial color from defaults/config
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
