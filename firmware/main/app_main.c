#include "dv_board.h"
#include "dv_button.h"
#include "dc_evlog.h"
#include "dc_bambu.h"
#include "dc_moonraker.h"
#include "dc_source.h"
#include "dv_motor.h"
#include "dv_policy.h"
#include "dv_portal.h"
#include "dv_status_led.h"
#include "dv_rgb.h"
#include "dc_wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "dragonvent";

static esp_err_t configure_network_identity(void)
{
    const dc_wifi_identity_t identity = {
        .hostname = "dragonvent",
        .instance_name = "DragonVent",
        .ap_ssid_prefix = "DragonVent_",
        .ap_password = DC_WIFI_DEFAULT_AP_PASSWORD,
    };
    return dc_wifi_set_identity(&identity);
}

static esp_err_t start_control_source(void)
{
    dc_ctl_source_t source = dc_source_get();
    ESP_LOGI(TAG, "control source: %s", dc_source_str(source));
    dc_evlog_add("control source: %s", dc_source_str(source));

    switch (source) {
    case DC_SRC_BAMBU:
        return dc_bambu_start();
    case DC_SRC_KLIPPER:
        return dc_moonraker_start();
    case DC_SRC_HA:
        ESP_LOGW(TAG, "Home Assistant source is not available in DragonVent yet");
        return ESP_OK;
    case DC_SRC_KLIPPER_MQTT:
        ESP_LOGW(TAG, "Klipper MQTT source is not available in DragonVent yet");
        return ESP_OK;
    case DC_SRC_NONE:
        return ESP_OK;
    case DC_SRC_MAX:
        break;
    }
    return ESP_ERR_INVALID_STATE;
}

static dv_motor_target_t flip(dv_motor_target_t t)
{
    return (t == DV_MOTOR_TARGET_OPEN) ? DV_MOTOR_TARGET_CLOSED : DV_MOTOR_TARGET_OPEN;
}

static void reflect_mode_on_led(void)
{
    // Match stock: LED off in AUTO, blinking in MANUAL.
    dv_status_led_set(dv_policy_get_mode() == DV_POLICY_MODE_AUTO
                          ? DV_STATUS_LED_OFF
                          : DV_STATUS_LED_BLINK);
}

// Feed current vent + printer state to the RGB lighting policy (which decides
// the color from the user's config: vent state, printing, or a temp gradient).
static void update_rgb_from_state(void)
{
    dv_printer_status_t status = DV_PS_NONE;
    float bed = NAN;
    switch (dc_source_get()) {
    case DC_SRC_BAMBU: {
        dc_bambu_status_t st = {0};
        dc_bambu_get_status(&st);
        status = st.error ? DV_PS_ERROR
               : st.printing ? DV_PS_PRINTING
               : st.connected ? DV_PS_IDLE : DV_PS_NONE;   // Bambu exposes a coarse status
        bed = st.bed_temp;
        break;
    }
    case DC_SRC_KLIPPER: {
        dc_moonraker_status_t st = {0};
        dc_moonraker_get_status(&st);
        switch (st.printer) {
        case DC_PRINTER_IDLE:      status = DV_PS_IDLE; break;
        case DC_PRINTER_PREPARING: status = DV_PS_PREPARING; break;
        case DC_PRINTER_PRINTING:  status = DV_PS_PRINTING; break;
        case DC_PRINTER_PAUSED:    status = DV_PS_PAUSED; break;
        case DC_PRINTER_COMPLETE:  status = DV_PS_COMPLETE; break;
        case DC_PRINTER_ERROR:     status = DV_PS_ERROR; break;
        default:                   status = DV_PS_NONE; break;   // UNKNOWN / not subscribed
        }
        bed = st.bed_temp;
        break;
    }
    default:
        break;
    }
    dv_rgb_update((int)dv_policy_get_target(), status, bed);
}

// Button semantics from the stock firmware:
//   USER short click, AUTO   → switch to MANUAL and reverse the vent state
//   USER short click, MANUAL → toggle the vent state
//   USER long press (3 s)    → switch to AUTO
//   BOOT long press (3 s)    → factory reset (wipe NVS, reboot)
static void on_button(dv_button_id_t id, dv_button_event_t ev)
{
    if (id == DV_BUTTON_USER && ev == DV_BUTTON_SHORT) {
        dv_motor_target_t next = flip(dv_policy_get_target());
        dv_policy_set_manual_target(next);
        dv_policy_set_mode(DV_POLICY_MODE_MANUAL);
        reflect_mode_on_led();
        ESP_LOGI(TAG, "USER short: MANUAL, target=%d", next);
        return;
    }
    if (id == DV_BUTTON_USER && ev == DV_BUTTON_LONG) {
        // Stock's long-press callback unconditionally goes to AUTO
        // (FUN_400de994(0)); no toggle.
        dv_policy_set_mode(DV_POLICY_MODE_AUTO);
        reflect_mode_on_led();
        ESP_LOGI(TAG, "USER long: mode=AUTO");
        return;
    }
    if (id == DV_BUTTON_BOOT && ev == DV_BUTTON_LONG) {
        ESP_LOGW(TAG, "BOOT long: factory reset");
        dc_wifi_clear_creds();
        dc_moonraker_clear_config();
        dc_bambu_clear_config();
        dc_source_set(DC_SRC_KLIPPER);
        dv_policy_clear();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

// Device-specific control-source selection after an OTA-over-stock. dc_wifi's
// migrate (core) already carries all stock config across — WiFi, Moonraker, HA, and
// the bound Bambu printer (bb_host/bb_serial/bb_code). Choosing which source to run
// is device policy, not core's: the stock Panda Vent is a Bambu device, so on first
// boot (no source persisted yet) adopt Bambu if a printer was carried. Guarded on the
// ctl_src key being absent, so it fires exactly once and never overrides a later choice.
static void select_migrated_source(void)
{
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t src = 0;
    bool already_chosen = (nvs_get_u8(h, "ctl_src", &src) == ESP_OK);
    nvs_close(h);
    if (already_chosen) return;

    dc_bambu_config_t bb = {0};
    dc_bambu_get_config(&bb);
    if (bb.host[0]) {
        dc_source_set(DC_SRC_BAMBU);   // persists ctl_src, so this runs only once
        ESP_LOGW(TAG, "carried Bambu printer %s; control source -> Bambu", bb.host);
        dc_evlog_add("migrated source -> Bambu");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DragonVent booting");

    dc_evlog_console_init();
    dc_evlog_init();
    dc_evlog_add("DragonVent boot");

    ESP_ERROR_CHECK(dv_motor_init());
    ESP_ERROR_CHECK(configure_network_identity());
    ESP_ERROR_CHECK(dc_wifi_start());
    select_migrated_source();   // device-specific: adopt a stock-bound Bambu printer
    ESP_ERROR_CHECK(start_control_source());
    ESP_ERROR_CHECK(dv_policy_start());
    ESP_ERROR_CHECK(dv_portal_start());
    ESP_ERROR_CHECK(dv_status_led_start());
    reflect_mode_on_led();
    // WS2812 strips: init after the motor (ADC+LEDC) so RMT comes up last, per
    // stock ordering. Non-fatal — a strip failure must not take down the vent.
    esp_err_t rgb_err = dv_rgb_start();
    if (rgb_err != ESP_OK) {
        ESP_LOGW(TAG, "dv_rgb_start failed: %s (continuing without strip LEDs)",
                 esp_err_to_name(rgb_err));
    }
    update_rgb_from_state();
    ESP_ERROR_CHECK(dv_button_start(on_button));

    // Mirror mode changes onto the button LED, and refresh the strip color from
    // live state each tick (printing/temp change independently of the target;
    // dv_rgb skips the RMT write when the resolved color is unchanged).
    dv_policy_mode_t last_mode = dv_policy_get_mode();
    for (;;) {
        dv_policy_mode_t m = dv_policy_get_mode();
        if (m != last_mode) {
            reflect_mode_on_led();
            last_mode = m;
        }
        update_rgb_from_state();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
