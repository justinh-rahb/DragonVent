#include "dv_rgb.h"
#include "dv_board.h"
#include "dv_motor.h"

#include "led_strip.h"
#include "led_strip_spi.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#define FRAME_MS        30    // ~33 fps animation refresh

#define CFG_NVS_NS   "app_nvs"
#define CFG_NVS_KEY  "lighting"

static const gpio_num_t STRIP_GPIO[MAX_STRIPS] = {
    DV_PIN_RGB_STRIP_0,   // GPIO 14
    DV_PIN_RGB_STRIP_1,   // GPIO 4 (2-vent kit only)
};

static led_strip_handle_t s_strips[MAX_STRIPS];
static int                s_count = 0;
static SemaphoreHandle_t  s_lock  = NULL;
static TaskHandle_t       s_task  = NULL;

// Lighting config (defaults keep the validated blue/red behavior; printing/temp
// and animation are opt-in so nothing changes until the user enables them).
static dv_lighting_t s_cfg = {
    .enabled = true, .brightness = 255,
    .open    = {0, 0, 255},   // blue = cool
    .closed  = {255, 0, 0},   // red = hot
    .printing = {0, 255, 0},  // green = active
    .use_printing = false, .use_temp = false,
    .temp_min_c = 25, .temp_max_c = 60,
    .effect = DV_FX_SOLID, .speed = 128,
    .error = {255, 0, 0}, .use_error = false,   // flashing red on a print error
    // Printer-status mode (defaults mirror the stock Panda Vent palette).
    .mode = DV_LIGHT_MODE_VENT,
    .idle     = {255, 255, 255},   // white
    .prep     = {248, 163,  35},   // orange (stock F8A323)
    .paused   = {255, 255, 255},   // white
    .complete = {  0, 255,  42},   // green (stock 00FF2A)
};

// Latest state fed to the policy, the animation phase/frame counter, and the
// last solid color pushed (so a steady color skips redundant RMT refreshes).
static int      s_target   = DV_MOTOR_TARGET_CLOSED;
static int      s_pstatus  = DV_PS_NONE;
static bool     s_printing = false;
static bool     s_error    = false;
static float    s_bed      = NAN;
static uint32_t s_phase    = 0;
static uint32_t s_frames   = 0;
static uint8_t  s_last[3]  = {1, 2, 3};   // impossible first value forces a push

static esp_err_t make_strip(int index, gpio_num_t gpio, led_strip_handle_t *out)
{
    led_strip_config_t strip = {
        .strip_gpio_num   = gpio,
        .max_leds         = LEDS_PER_STRIP,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags            = { .invert_out = false },
    };
    // Drive WS2812 over SPI + DMA rather than RMT. The classic ESP32's RMT has no
    // DMA, so its refill ISR can be starved by a WiFi/flash-cache-disable stall
    // mid-frame — corrupting bits into a one-frame bright/white flicker. SPI
    // streams the whole frame from RAM by DMA with no per-frame ISR, so it can't
    // be starved. One SPI host per strip (this board leaves both HSPI+VSPI free);
    // MOSI routes to the strip GPIO via the GPIO matrix, fine at WS2812's ~2.5 MHz.
    led_strip_spi_config_t spi = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = (index == 0) ? SPI2_HOST : SPI3_HOST,
        .flags   = { .with_dma = true },
    };
    return led_strip_new_spi_device(&strip, &spi, out);
}

// h: 0-65535, s/v: 0-255.
static void hsv2rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t out[3])
{
    uint8_t region = h / 10923;                 // 65536 / 6
    uint16_t rem = (uint16_t)((h - region * 10923) * 6);
    uint8_t p = (uint8_t)((v * (255 - s)) / 255);
    uint8_t q = (uint8_t)((v * (255 - (s * rem) / 65535)) / 255);
    uint8_t t = (uint8_t)((v * (255 - (s * (65535 - rem)) / 65535)) / 255);
    switch (region) {
    case 0:  out[0]=v; out[1]=t; out[2]=p; break;
    case 1:  out[0]=q; out[1]=v; out[2]=p; break;
    case 2:  out[0]=p; out[1]=v; out[2]=t; break;
    case 3:  out[0]=p; out[1]=q; out[2]=v; break;
    case 4:  out[0]=t; out[1]=p; out[2]=v; break;
    default: out[0]=v; out[1]=p; out[2]=q; break;
    }
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

// Map the printer status to its configured color (PRINTER mode).
static const uint8_t *printer_status_color(void)
{
    switch (s_pstatus) {
    case DV_PS_PREPARING: return s_cfg.prep;
    case DV_PS_PRINTING:  return s_cfg.printing;
    case DV_PS_PAUSED:    return s_cfg.paused;
    case DV_PS_COMPLETE:  return s_cfg.complete;
    case DV_PS_ERROR:     return s_cfg.error;
    default:              return s_cfg.idle;   // IDLE / NONE
    }
}

// Resolve the state base color (no brightness). PRINTER mode follows the printer
// status; VENT mode follows the vent (open/closed, printing override, temp).
static void compute_base(uint8_t out[3])
{
    const uint8_t *base;
    uint8_t grad[3];
    if (s_cfg.mode == DV_LIGHT_MODE_PRINTER) {
        base = printer_status_color();
    } else if (s_cfg.use_printing && s_printing) {
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
    out[0] = base[0]; out[1] = base[1]; out[2] = base[2];
}

// Render one frame from the current config + state (assumes s_lock held).
static void render_locked(void)
{
    if (!s_cfg.enabled) {
        const uint8_t off[3] = {0, 0, 0};
        if (memcmp(off, s_last, 3) != 0) { fill_locked(off); memcpy(s_last, off, 3); }
        return;
    }

    // A print error takes top precedence and flashes to demand attention,
    // overriding whatever effect is selected.
    if (s_cfg.use_error && s_error) {
        bool on = ((s_frames / 25) & 1) == 0;   // ~0.75 s on / 0.75 s off
        uint8_t rgb[3] = {0, 0, 0};
        if (on) for (int k = 0; k < 3; ++k) rgb[k] = (uint8_t)((uint16_t)s_cfg.error[k] * s_cfg.brightness / 255);
        fill_locked(rgb);
        s_last[0] = 0x55;   // invalidate solid dedup
        return;
    }

    switch (s_cfg.effect) {
    case DV_FX_CYCLE: {
        uint8_t rgb[3];
        hsv2rgb((uint16_t)s_phase, 255, s_cfg.brightness, rgb);
        fill_locked(rgb);                     // animated -> always refresh
        s_last[0] = ~rgb[0];                  // invalidate solid dedup
        break;
    }
    case DV_FX_RAINBOW: {
        for (int i = 0; i < s_count; ++i) {
            for (int j = 0; j < LEDS_PER_STRIP; ++j) {
                int p = s_cfg.reverse ? (LEDS_PER_STRIP - 1 - j) : j;
                uint16_t hue = (uint16_t)(s_phase + (uint32_t)p * (65536 / LEDS_PER_STRIP));
                uint8_t rgb[3];
                hsv2rgb(hue, 255, s_cfg.brightness, rgb);
                led_strip_set_pixel(s_strips[i], j, rgb[0], rgb[1], rgb[2]);
            }
            led_strip_refresh(s_strips[i]);
        }
        s_last[0] = 0xAA;                      // invalidate solid dedup
        break;
    }
    case DV_FX_STROBE: {
        uint8_t base[3];
        compute_base(base);
        bool on = ((s_phase >> 12) & 1) == 0;   // flash rate scales with speed
        uint8_t rgb[3] = {0, 0, 0};
        if (on) for (int k = 0; k < 3; ++k) rgb[k] = (uint8_t)((uint16_t)base[k] * s_cfg.brightness / 255);
        fill_locked(rgb);
        s_last[0] = 0x33;
        break;
    }
    case DV_FX_WAVE: {
        uint8_t base[3];
        compute_base(base);
        for (int i = 0; i < s_count; ++i) {
            for (int j = 0; j < LEDS_PER_STRIP; ++j) {
                int p = s_cfg.reverse ? (LEDS_PER_STRIP - 1 - j) : j;
                uint16_t x = (uint16_t)((uint32_t)p * (65536 / LEDS_PER_STRIP) + s_phase);
                uint8_t w = (x < 32768) ? (uint8_t)(x >> 7) : (uint8_t)(255 - ((x - 32768) >> 7));
                uint16_t lvl = (uint16_t)s_cfg.brightness * w / 255;
                uint8_t rgb[3];
                for (int k = 0; k < 3; ++k) rgb[k] = (uint8_t)((uint16_t)base[k] * lvl / 255);
                led_strip_set_pixel(s_strips[i], j, rgb[0], rgb[1], rgb[2]);
            }
            led_strip_refresh(s_strips[i]);
        }
        s_last[0] = 0x44;
        break;
    }
    case DV_FX_MARQUEE: {
        uint8_t base[3];
        compute_base(base);
        uint8_t on_rgb[3];
        for (int k = 0; k < 3; ++k) on_rgb[k] = (uint8_t)((uint16_t)base[k] * s_cfg.brightness / 255);
        int offset = (int)((s_phase >> 11) % 3);
        for (int i = 0; i < s_count; ++i) {
            for (int j = 0; j < LEDS_PER_STRIP; ++j) {
                int p = s_cfg.reverse ? (LEDS_PER_STRIP - 1 - j) : j;
                bool lit = ((p + offset) % 3) == 0;   // every 3rd LED, scrolling
                if (lit) led_strip_set_pixel(s_strips[i], j, on_rgb[0], on_rgb[1], on_rgb[2]);
                else     led_strip_set_pixel(s_strips[i], j, 0, 0, 0);
            }
            led_strip_refresh(s_strips[i]);
        }
        s_last[0] = 0x66;
        break;
    }
    case DV_FX_BREATHE: {
        uint8_t base[3];
        compute_base(base);
        uint16_t ph = (uint16_t)s_phase;       // triangle wave 0..255..0
        uint8_t wave = (ph < 32768) ? (uint8_t)(ph >> 7) : (uint8_t)(255 - ((ph - 32768) >> 7));
        // never fully black out: floor at ~15%.
        uint16_t lvl = (uint16_t)s_cfg.brightness * (39 + (uint16_t)wave * 216 / 255) / 255;
        uint8_t rgb[3];
        for (int k = 0; k < 3; ++k) rgb[k] = (uint8_t)((uint16_t)base[k] * lvl / 255);
        fill_locked(rgb);
        s_last[0] = ~rgb[0];
        break;
    }
    case DV_FX_SOLID:
    default: {
        uint8_t base[3];
        compute_base(base);
        uint8_t rgb[3];
        for (int k = 0; k < 3; ++k) rgb[k] = (uint8_t)((uint16_t)base[k] * s_cfg.brightness / 255);
        if (memcmp(rgb, s_last, 3) != 0) { fill_locked(rgb); memcpy(s_last, rgb, 3); }
        break;
    }
    }
}

static void anim_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        render_locked();
        s_frames++;
        if (s_cfg.enabled && s_cfg.effect != DV_FX_SOLID) {
            s_phase += 8 + (uint32_t)s_cfg.speed * 4;   // hue units per frame
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void dv_rgb_update(int target, int status, float bed_temp_c)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target = target;
    s_pstatus = status;
    s_printing = (status == DV_PS_PRINTING);   // vent-mode printing override
    s_error = (status == DV_PS_ERROR);         // error-flash override (both modes)
    s_bed = bed_temp_c;
    xSemaphoreGive(s_lock);
    // The animation task renders on the next frame.
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
    s_last[0] = 0x7F; s_last[1] = 0x3F; s_last[2] = 0x1F;   // force a repaint
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, CFG_NVS_KEY, &s_cfg, sizeof(s_cfg));
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

// Tolerant load: a shorter stored blob (an older config layout) is copied into
// the front of s_cfg, leaving any newer fields at their compiled defaults.
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t need = 0;
    if (nvs_get_blob(h, CFG_NVS_KEY, NULL, &need) == ESP_OK && need > 0) {
        uint8_t buf[sizeof(s_cfg)];
        size_t cap = sizeof(buf);
        if (need <= cap && nvs_get_blob(h, CFG_NVS_KEY, buf, &cap) == ESP_OK) {
            memcpy(&s_cfg, buf, cap);
            ESP_LOGI(TAG, "loaded lighting config (%u B) from NVS", (unsigned)cap);
        }
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
        esp_err_t err = make_strip(i, STRIP_GPIO[i], &s_strips[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "strip %d (GPIO %d) init failed: %s",
                     i, STRIP_GPIO[i], esp_err_to_name(err));
            return err;
        }
        led_strip_clear(s_strips[i]);
        ++s_count;
    }
    ESP_LOGI(TAG, "initialized %d WS2812 strip(s), %d LEDs each", s_count, LEDS_PER_STRIP);

    if (xTaskCreate(anim_task, "dv_rgb", 3072, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
