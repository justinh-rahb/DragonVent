// SPDX-License-Identifier: MIT
#include "dv_portal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_moonraker.h"
#include "dc_portal.h"
#include "dc_source.h"
#include "dc_wifi.h"
#include "dv_motor.h"
#include "dv_policy.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static uint32_t s_api_revision = 1;

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t api_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "error", "invalid_request");
    cJSON_AddStringToObject(root, "message", message);
    return send_json(req, root);
}

static cJSON *recv_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *text = malloc((size_t)req->content_len + 1);
    if (!text) return NULL;
    int offset = 0;
    while (offset < req->content_len) {
        int got = httpd_req_recv(req, text + offset, req->content_len - offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { free(text); return NULL; }
        offset += got;
    }
    text[offset] = 0;
    cJSON *root = cJSON_Parse(text);
    free(text);
    return root;
}

static void add_device_id(cJSON *root)
{
    uint8_t mac[6] = {0};
    char id[24] = "dragonvent";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(id, sizeof(id), "dragonvent-%02x%02x%02x", mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device_id", id);
}

static const char *target_wire(dv_motor_target_t target)
{
    return target == DV_MOTOR_TARGET_OPEN ? "open" :
           target == DV_MOTOR_TARGET_CLOSED ? "closed" : "stop";
}

static const char *wifi_wire(dc_wifi_state_t state)
{
    switch (state) {
    case DC_WIFI_STATE_INIT: return "starting";
    case DC_WIFI_STATE_STA_CONNECTING: return "connecting";
    case DC_WIFI_STATE_STA_CONNECTED: return "station";
    case DC_WIFI_STATE_AP_PORTAL: return "setup_ap";
    }
    return "unknown";
}

static cJSON *make_state(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "project", "dragonvent");
    cJSON_AddNumberToObject(root, "state_revision", s_api_revision);
    cJSON_AddStringToObject(root, "firmware", app->version);
    add_device_id(root);
    cJSON_AddStringToObject(root, "mode", dv_policy_get_mode() == DV_POLICY_MODE_AUTO ? "auto" : "manual");

    int groups = dv_motor_active_groups();
    bool running = false;
    for (int i = 0; i < groups; ++i) running |= dv_motor_is_running(i);
    cJSON *vent = cJSON_AddObjectToObject(root, "vent");
    cJSON_AddStringToObject(vent, "target", target_wire(dv_policy_get_target()));
    cJSON_AddBoolToObject(vent, "running", running);
    cJSON_AddNumberToObject(vent, "active_groups", groups);

    dc_ctl_source_t source = dc_source_get();
    cJSON *printer = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddStringToObject(printer, "source", dc_source_str(source));
    bool connected = false;
    const char *printer_state = source == DC_SRC_NONE ? "standalone" : "unknown";
    float bed = NAN;
    const char *material = "";
    if (source == DC_SRC_KLIPPER) {
        dc_moonraker_status_t status = {0};
        dc_moonraker_get_status(&status);
        connected = status.state == DC_MK_SUBSCRIBED;
        printer_state = dc_printer_state_str(status.printer);
        bed = status.bed_temp;
        material = status.material;
    } else if (source == DC_SRC_BAMBU) {
        dc_bambu_status_t status = {0};
        dc_bambu_get_status(&status);
        connected = status.connected;
        printer_state = status.printing ? "printing" : "idle";
        bed = status.bed_temp;
        material = status.filament;
    }
    cJSON_AddBoolToObject(printer, "connected", connected);
    cJSON_AddStringToObject(printer, "state", printer_state);
    if (isnan(bed)) cJSON_AddNullToObject(printer, "bed_temperature_c");
    else cJSON_AddNumberToObject(printer, "bed_temperature_c", bed);
    cJSON_AddStringToObject(printer, "material", material);

    float open_c = 45, close_c = 35;
    dv_policy_get_thresholds(&open_c, &close_c);
    cJSON *policy = cJSON_AddObjectToObject(root, "policy");
    cJSON_AddNumberToObject(policy, "bed_open_c", open_c);
    cJSON_AddNumberToObject(policy, "bed_close_c", close_c);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "state", wifi_wire(dc_wifi_state()));
    if (dc_wifi_state() == DC_WIFI_STATE_STA_CONNECTED) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {0};
        if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
            char ip[20];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
            cJSON_AddStringToObject(wifi, "ip", ip);
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    }
    return root;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "project", "dragonvent");
    add_device_id(root);
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    const char *values[] = { "vent_manual", "vent_auto", "source_status", "polling", "provisioning" };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        cJSON_AddItemToArray(caps, cJSON_CreateString(values[i]));
    cJSON *ui = cJSON_AddObjectToObject(root, "ui");
    cJSON_AddNumberToObject(ui, "schema", 1);
    cJSON_AddStringToObject(ui, "product", "dragonvent");
    cJSON_AddStringToObject(ui, "display_name", "DragonVent");
    // Opt into the shared SPA's update check (dragon-core >= v0.7.0). It asks
    // GitHub for the latest stable release of this repo and, when newer, shows
    // the version, the expected SHA-256 and a download link — it never
    // auto-flashes. The SPA picks the asset by prefix, excluding any name
    // containing "factory", so this resolves to dragonvent-<tag>-ota.bin (the
    // web-uploadable app image) and never the full USB image. Local/dev builds
    // skip the request entirely, so this is silent until a tagged build runs.
    cJSON *update = cJSON_AddObjectToObject(root, "update");
    cJSON_AddStringToObject(update, "repo", "justinh-rahb/DragonVent");
    cJSON_AddStringToObject(update, "asset_prefix", "dragonvent-");
    return send_json(req, root);
}

static esp_err_t state_get(httpd_req_t *req) { return send_json(req, make_state()); }

static esp_err_t command_post(httpd_req_t *req)
{
    cJSON *body = recv_json(req);
    cJSON *command = body ? cJSON_GetObjectItemCaseSensitive(body, "command") : NULL;
    cJSON *name = command ? cJSON_GetObjectItemCaseSensitive(command, "name") : NULL;
    if (!cJSON_IsString(name)) { cJSON_Delete(body); return api_error(req, "400 Bad Request", "missing command name"); }
    esp_err_t err = ESP_OK;
    if (!strcmp(name->valuestring, "auto")) {
        err = dv_policy_set_mode(DV_POLICY_MODE_AUTO);
    } else if (!strcmp(name->valuestring, "manual")) {
        cJSON *target = cJSON_GetObjectItemCaseSensitive(command, "target");
        if (!cJSON_IsString(target) || (strcmp(target->valuestring, "open") && strcmp(target->valuestring, "closed"))) {
            cJSON_Delete(body); return api_error(req, "400 Bad Request", "manual target must be open or closed");
        }
        dv_motor_target_t value = !strcmp(target->valuestring, "open") ? DV_MOTOR_TARGET_OPEN : DV_MOTOR_TARGET_CLOSED;
        err = dv_policy_set_mode(DV_POLICY_MODE_MANUAL);
        if (err == ESP_OK) err = dv_policy_set_manual_target(value);
    } else {
        cJSON_Delete(body); return api_error(req, "400 Bad Request", "unknown command");
    }
    cJSON_Delete(body);
    if (err != ESP_OK) return api_error(req, "409 Conflict", esp_err_to_name(err));
    ++s_api_revision;
    dc_evlog_add("api: mode=%s target=%s", dv_policy_get_mode() == DV_POLICY_MODE_AUTO ? "auto" : "manual", target_wire(dv_policy_get_target()));
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "state", make_state());
    return send_json(req, reply);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    float open_c = 45, close_c = 35;
    dv_policy_get_thresholds(&open_c, &close_c);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddNumberToObject(root, "bed_open_c", open_c);
    cJSON_AddNumberToObject(root, "bed_close_c", close_c);
    return send_json(req, root);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *body = recv_json(req);
    cJSON *open = body ? cJSON_GetObjectItemCaseSensitive(body, "bed_open_c") : NULL;
    cJSON *close = body ? cJSON_GetObjectItemCaseSensitive(body, "bed_close_c") : NULL;
    if (!cJSON_IsNumber(open) || !cJSON_IsNumber(close)) { cJSON_Delete(body); return api_error(req, "400 Bad Request", "bed_open_c and bed_close_c are required"); }
    float open_c = (float)open->valuedouble, close_c = (float)close->valuedouble;
    cJSON_Delete(body);
    if (!isfinite(open_c) || !isfinite(close_c) || close_c < 0 || open_c > 120 || dv_policy_set_thresholds(open_c, close_c) != ESP_OK)
        return api_error(req, "400 Bad Request", "open temperature must be above close temperature and both must be 0..120 C");
    ++s_api_revision;
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "state", make_state());
    return send_json(req, reply);
}

static void url_decode(char *text)
{
    char *out = text;
    for (char *p = text; *p; ++p) {
        if (*p == '+') *out++ = ' ';
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 }; *out++ = (char)strtol(hex, NULL, 16); p += 2;
        } else *out++ = *p;
    }
    *out = 0;
}

static esp_err_t tasmota_get(httpd_req_t *req)
{
    char query[128] = {0}, command[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "cmnd", command, sizeof(command));
    url_decode(command);
    const char *arg = command;
    if (!strncasecmp(arg, "Power", 5)) { arg += 5; while (isdigit((unsigned char)*arg)) ++arg; while (*arg == ' ') ++arg; }
    dv_motor_target_t current = dv_policy_get_target();
    bool on = current == DV_MOTOR_TARGET_OPEN;
    if (!strcasecmp(arg, "ON")) on = true;
    else if (!strcasecmp(arg, "OFF")) on = false;
    else if (!strcasecmp(arg, "TOGGLE")) on = !on;
    else goto respond;
    dv_policy_set_mode(DV_POLICY_MODE_MANUAL);
    dv_policy_set_manual_target(on ? DV_MOTOR_TARGET_OPEN : DV_MOTOR_TARGET_CLOSED);
    dc_evlog_add("tasmota: POWER %s", on ? "ON" : "OFF");
respond:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, on ? "{\"POWER\":\"ON\"}" : "{\"POWER\":\"OFF\"}");
}

static cJSON *field(cJSON *fields, const char *key, const char *label, const char *type, const char *value)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "key", key);
    cJSON_AddStringToObject(item, "label", label);
    cJSON_AddStringToObject(item, "type", type);
    if (value) cJSON_AddStringToObject(item, "value", value);
    cJSON_AddItemToArray(fields, item);
    return item;
}

static cJSON *describe_product(void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_CreateObject(), *sections = cJSON_AddArrayToObject(root, "sections");
    cJSON *printer = cJSON_CreateObject();
    cJSON_AddStringToObject(printer, "title", "Printer source");
    cJSON_AddStringToObject(printer, "description", "Choose one controller. Source changes take effect after restart.");
    cJSON *fields = cJSON_AddArrayToObject(printer, "fields");
    cJSON *source = field(fields, "source", "Control source", "select", dc_source_str(dc_source_get()));
    cJSON *options = cJSON_AddArrayToObject(source, "options");
    const char *source_values[][2] = {{"klipper","Klipper / Moonraker"},{"bambu","Bambu LAN"},{"none","Standalone"}};
    for (size_t i = 0; i < 3; ++i) { cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o,"value",source_values[i][0]); cJSON_AddStringToObject(o,"label",source_values[i][1]); cJSON_AddItemToArray(options,o); }
    dc_moonraker_config_t mk = {0}; dc_moonraker_get_config(&mk);
    char port[8]; snprintf(port, sizeof(port), "%u", mk.port ?: 7125);
    field(fields, "moonraker_host", "Moonraker host", "text", mk.host);
    field(fields, "moonraker_port", "Moonraker port", "number", port);
    cJSON_AddBoolToObject(field(fields, "moonraker_api_key", "Moonraker API key", "text", ""), "secret", true);
    dc_bambu_config_t bb = {0}; dc_bambu_get_config(&bb);
    field(fields, "bambu_host", "Bambu host", "text", bb.host);
    field(fields, "bambu_serial", "Bambu serial", "text", bb.serial);
    cJSON_AddBoolToObject(field(fields, "bambu_code", "Bambu access code", "text", ""), "secret", true);
    cJSON_AddItemToArray(sections, printer);

    float open_c = 45, close_c = 35; dv_policy_get_thresholds(&open_c, &close_c);
    cJSON *policy = cJSON_CreateObject(); cJSON_AddStringToObject(policy, "title", "Automatic vent policy");
    fields = cJSON_AddArrayToObject(policy, "fields");
    char number[16]; snprintf(number, sizeof(number), "%.0f", open_c);
    cJSON *f = field(fields, "bed_open_c", "Open at °C", "number", number); cJSON_AddNumberToObject(f,"min",1); cJSON_AddNumberToObject(f,"max",120);
    snprintf(number, sizeof(number), "%.0f", close_c);
    f = field(fields, "bed_close_c", "Close below °C", "number", number); cJSON_AddNumberToObject(f,"min",0); cJSON_AddNumberToObject(f,"max",119);
    cJSON_AddItemToArray(sections, policy);
    return root;
}

static const char *string_value(const cJSON *values, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(values, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool number_value(const cJSON *values, const char *key, double *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(values, key);
    if (cJSON_IsNumber(item)) { *out = item->valuedouble; return true; }
    if (cJSON_IsString(item) && item->valuestring[0]) { *out = strtod(item->valuestring, NULL); return true; }
    return false;
}

static esp_err_t apply_product(const cJSON *values, void *ctx, char *message, size_t message_size)
{
    (void)ctx;
    const char *source_text = string_value(values, "source");
    dc_ctl_source_t source = dc_source_get();
    if (source_text) {
        if (!strcmp(source_text, "klipper")) source = DC_SRC_KLIPPER;
        else if (!strcmp(source_text, "bambu")) source = DC_SRC_BAMBU;
        else if (!strcmp(source_text, "none")) source = DC_SRC_NONE;
        else { snprintf(message, message_size, "Unknown control source"); return ESP_ERR_INVALID_ARG; }
        esp_err_t err = dc_source_set(source);
        if (err != ESP_OK) return err;
    }
    const char *mk_host = string_value(values, "moonraker_host");
    if (mk_host) {
        dc_moonraker_config_t config = {0}; dc_moonraker_get_config(&config);
        snprintf(config.host, sizeof(config.host), "%s", mk_host);
        double port = 0;
        if (number_value(values, "moonraker_port", &port)) { long parsed = (long)port; if (parsed < 1 || parsed > 65535) { snprintf(message,message_size,"Invalid Moonraker port"); return ESP_ERR_INVALID_ARG; } config.port = (uint16_t)parsed; }
        const char *key = string_value(values, "moonraker_api_key"); if (key && *key) snprintf(config.api_key, sizeof(config.api_key), "%s", key);
        esp_err_t err = dc_moonraker_set_config(&config);
        if (err != ESP_OK) return err;
    }
    const char *bb_host = string_value(values, "bambu_host");
    if (bb_host) {
        dc_bambu_config_t config = {0}; dc_bambu_get_config(&config);
        snprintf(config.host, sizeof(config.host), "%s", bb_host);
        const char *serial = string_value(values, "bambu_serial"); if (serial) snprintf(config.serial, sizeof(config.serial), "%s", serial);
        const char *code = string_value(values, "bambu_code"); if (code && *code) snprintf(config.code, sizeof(config.code), "%s", code);
        esp_err_t err = dc_bambu_set_config(&config);
        if (err != ESP_OK) return err;
    }
    double open_c = 0, close_c = 0;
    if (number_value(values, "bed_open_c", &open_c) && number_value(values, "bed_close_c", &close_c) &&
        dv_policy_set_thresholds((float)open_c, (float)close_c) != ESP_OK) {
        snprintf(message, message_size, "Open temperature must be above close temperature"); return ESP_ERR_INVALID_ARG;
    }
    snprintf(message, message_size, "Settings saved. Restart to apply a source change.");
    return ESP_OK;
}

static esp_err_t factory_reset(void *ctx)
{
    (void)ctx;
    esp_err_t first = dc_moonraker_clear_config();
    if (first == ESP_OK) first = dc_bambu_clear_config();
    if (first == ESP_OK) first = dc_source_set(DC_SRC_KLIPPER);
    if (first == ESP_OK) first = dv_policy_clear();
    return first;
}

esp_err_t dv_portal_start(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/v2/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/v2/state", .method = HTTP_GET, .handler = state_get },
        { .uri = "/api/v2/command", .method = HTTP_POST, .handler = command_post },
        { .uri = "/api/v2/settings", .method = HTTP_GET, .handler = settings_get },
        { .uri = "/api/v2/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/cm", .method = HTTP_GET, .handler = tasmota_get },
    };
    const dc_portal_config_t config = {
        .product = "dragonvent", .display_name = "DragonVent",
        .product_routes = routes, .product_route_count = sizeof(routes) / sizeof(routes[0]),
        .describe_product = describe_product, .apply_product = apply_product,
        .factory_reset = factory_reset,
    };
    return dc_portal_start(&config);
}

esp_err_t dv_portal_stop(void) { return dc_portal_stop(); }
