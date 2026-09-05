// SPDX-License-Identifier: MIT
#include "dv_peer.h"

#include "dc_peer.h"
#include "dc_source.h"
#include "dc_moonraker.h"
#include "dc_bambu.h"
#include "dv_policy.h"
#include "dv_motor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "dv_peer";

#define STATUS_MS    2000    // vent status heartbeat
#define ANNOUNCE_MS  6000    // descriptor heartbeat (every 3rd status tick)

// Map the Vent's control-source printer state to the compact peer summary (display only).
static uint8_t printer_summary(void)
{
    switch (dc_source_get()) {
        case DC_SRC_KLIPPER: {
            dc_moonraker_status_t st = {0};
            if (dc_moonraker_get_status(&st) != ESP_OK) return DC_PEER_PRINTER_UNKNOWN;
            switch (st.printer) {
                case DC_PRINTER_IDLE:
                case DC_PRINTER_COMPLETE:  return DC_PEER_PRINTER_IDLE;
                case DC_PRINTER_PREPARING:
                case DC_PRINTER_PRINTING:  return DC_PEER_PRINTER_PRINTING;
                case DC_PRINTER_PAUSED:    return DC_PEER_PRINTER_PAUSED;
                case DC_PRINTER_ERROR:     return DC_PEER_PRINTER_ERROR;
                default:                   return DC_PEER_PRINTER_UNKNOWN;
            }
        }
        case DC_SRC_BAMBU: {
            dc_bambu_status_t st = {0};
            if (dc_bambu_get_status(&st) != ESP_OK) return DC_PEER_PRINTER_UNKNOWN;
            return st.printing ? DC_PEER_PRINTER_PRINTING : DC_PEER_PRINTER_IDLE;
        }
        default:
            return DC_PEER_PRINTER_OFFLINE;   // Standalone / no source
    }
}

static bool any_running(void)
{
    int n = dv_motor_active_groups();
    for (int g = 0; g < n; ++g) if (dv_motor_is_running(g)) return true;
    return false;
}

static void fill_ip(uint8_t ip[4])
{
    memset(ip, 0, 4);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        uint32_t a = info.ip.addr;   // little-endian storage of the octet order
        ip[0] = a & 0xff; ip[1] = (a >> 8) & 0xff; ip[2] = (a >> 16) & 0xff; ip[3] = (a >> 24) & 0xff;
    }
}

static void publish_announce(void)
{
    dc_peer_announce_t a = {0};
    a.kind  = DC_PEER_KIND_VENT;
    a.flags = 0;
    a.caps  = DC_PEER_CAP_BIT(DC_PEER_CAP_ANNOUNCE) | DC_PEER_CAP_BIT(DC_PEER_CAP_VENT);
    fill_ip(a.ip);
    strlcpy(a.name, "DragonVent", sizeof(a.name));
    strlcpy(a.fw, esp_app_get_description()->version, sizeof(a.fw));   // truncates cleanly
    dc_peer_publish(DC_PEER_CAP_ANNOUNCE, &a, sizeof(a));
}

static void publish_status(void)
{
    dc_peer_vent_t v = {0};
    v.mode   = dv_policy_get_mode() == DV_POLICY_MODE_MANUAL ? 1 : 0;
    v.target = dv_policy_get_target() == DV_MOTOR_TARGET_CLOSED ? 1 : 0;
    v.flags  = (any_running() ? DC_PEER_VENT_RUNNING : 0)
             | (dv_motor_is_calibrating() ? DC_PEER_VENT_CALIBRATING : 0);
    v.printer_state = printer_summary();
    v.state_revision = 0;
    dc_peer_publish(DC_PEER_CAP_VENT, &v, sizeof(v));
}

static void peer_task(void *arg)
{
    (void)arg;
    int since_announce = ANNOUNCE_MS;   // announce immediately on first tick
    for (;;) {
        publish_status();
        since_announce += STATUS_MS;
        if (since_announce >= ANNOUNCE_MS) { publish_announce(); since_announce = 0; }
        vTaskDelay(pdMS_TO_TICKS(STATUS_MS));
    }
}

esp_err_t dv_peer_start(void)
{
    if (xTaskCreate(peer_task, "dv_peer", 3072, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "vent capability provider up (announce + status)");
    return ESP_OK;
}
