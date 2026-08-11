#include "dv_rgb.h"
#include "dv_board.h"
#include "dv_motor.h"

#include "led_strip.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "dv_rgb";

// Physical LED count varies (RE saw 16 on one board, 27 across two). Driving
// MORE than exist is harmless with a solid fill — surplus refresh words fall off
// the end of the chain — so use a generous per-strip count and drive both GPIO
// outputs. An unpopulated strip's GPIO just clocks out to nothing.
#define LEDS_PER_STRIP  30
#define MAX_STRIPS      2

static const gpio_num_t STRIP_GPIO[MAX_STRIPS] = {
    DV_PIN_RGB_STRIP_0,   // GPIO 14
    DV_PIN_RGB_STRIP_1,   // GPIO 4 (2-vent kit only)
};

static led_strip_handle_t s_strips[MAX_STRIPS];
static int                s_count = 0;
static SemaphoreHandle_t  s_lock  = NULL;

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

esp_err_t dv_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; ++i) {
        for (int j = 0; j < LEDS_PER_STRIP; ++j) {
            led_strip_set_pixel(s_strips[i], j, r, g, b);
        }
        led_strip_refresh(s_strips[i]);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
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
    return ESP_OK;
}
