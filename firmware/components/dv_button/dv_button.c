#include "dv_button.h"
#include "dv_board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

static const char *TAG = "dv_button";

#define TICK_MS            10
#define DEBOUNCE_TICKS     2      // 20 ms — settle time after a level change
#define LONG_PRESS_MS      3000   // stock v1.0.0 binary + shipped user manual
                                  // (BTT wiki says 6 s but v1.0.0 is the only
                                  // released FW — the wiki appears aspirational)

typedef struct {
    dv_button_id_t id;
    gpio_num_t     pin;
    bool           pressed;       // debounced state (true = actively held)
    int            hold_ticks;    // ticks since press started
    bool           long_fired;    // long-press callback already dispatched
    int            settle;        // ticks remaining before we trust a level change
    int            last_raw;      // most recent raw sample
} btn_t;

static btn_t s_btns[] = {
    { DV_BUTTON_USER, DV_PIN_USER_BUTTON, false, 0, false, 0, 1 },
    { DV_BUTTON_BOOT, DV_PIN_BOOT_BUTTON, false, 0, false, 0, 1 },
};
#define N_BTNS (sizeof(s_btns) / sizeof(s_btns[0]))

static dv_button_cb_t s_cb = NULL;
static TaskHandle_t   s_task = NULL;

static void configure_pin(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void step_button(btn_t *b)
{
    int raw = gpio_get_level(b->pin);   // 1 = idle (pull-up), 0 = pressed

    // Debounce: require a level to persist for DEBOUNCE_TICKS samples before
    // we accept a new state.
    if (raw != b->last_raw) {
        b->settle = DEBOUNCE_TICKS;
        b->last_raw = raw;
        return;
    }
    if (b->settle > 0) { b->settle--; return; }

    bool pressed_now = (raw == 0);

    if (pressed_now && !b->pressed) {
        // Press edge.
        b->pressed    = true;
        b->hold_ticks = 0;
        b->long_fired = false;
    } else if (!pressed_now && b->pressed) {
        // Release edge. If we already fired long-press, suppress the short.
        b->pressed = false;
        if (!b->long_fired && s_cb) {
            s_cb(b->id, DV_BUTTON_SHORT);
        }
    } else if (pressed_now) {
        b->hold_ticks++;
        if (!b->long_fired && b->hold_ticks * TICK_MS >= LONG_PRESS_MS) {
            b->long_fired = true;
            if (s_cb) s_cb(b->id, DV_BUTTON_LONG);
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (size_t i = 0; i < N_BTNS; ++i) step_button(&s_btns[i]);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t dv_button_start(dv_button_cb_t cb)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_cb = cb;
    for (size_t i = 0; i < N_BTNS; ++i) configure_pin(s_btns[i].pin);
    if (xTaskCreate(button_task, "dv_button", 3072, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "button handler running");
    return ESP_OK;
}
