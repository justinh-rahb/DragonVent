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
#include "dc_wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void app_main(void)
{
    ESP_LOGI(TAG, "DragonVent booting");

    dc_evlog_console_init();
    dc_evlog_init();
    dc_evlog_add("DragonVent boot");

    ESP_ERROR_CHECK(dv_motor_init());
    ESP_ERROR_CHECK(configure_network_identity());
    ESP_ERROR_CHECK(dc_wifi_start());
    ESP_ERROR_CHECK(start_control_source());
    ESP_ERROR_CHECK(dv_policy_start());
    ESP_ERROR_CHECK(dv_portal_start());
    ESP_ERROR_CHECK(dv_status_led_start());
    reflect_mode_on_led();
    ESP_ERROR_CHECK(dv_button_start(on_button));

    // Also mirror mode changes made via the web portal.
    dv_policy_mode_t last_mode = dv_policy_get_mode();
    for (;;) {
        dv_policy_mode_t m = dv_policy_get_mode();
        if (m != last_mode) {
            reflect_mode_on_led();
            last_mode = m;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
