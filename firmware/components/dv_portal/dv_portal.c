// SPDX-License-Identifier: MIT
#include "dv_portal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_moonraker.h"
#include "dc_portal.h"
#include "dc_source.h"
#include "dc_wifi.h"
#include "dv_motor.h"
#include "dv_rgb.h"
#include "dv_policy.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static uint32_t s_api_revision = 1;

// Control token. Same NVS namespace and key as DragonBreath so the family stays
// consistent, and the same two-tier model as pb_httpd_auth_ok():
//
//   token configured -> the header must match it exactly
//   no token         -> any non-empty header value passes
//
// The second tier is a CSRF gate rather than authentication: a cross-origin HTML
// form cannot set a custom header, so requiring one blocks the drive-by case
// while leaving an unconfigured device usable. Setting a token upgrades it to
// real authentication.
#define DV_NVS_NS       "app_nvs"
#define DV_NVS_TOKEN    "ctl_token"
#define DV_TOKEN_MAX    64
// dragon-core >= v0.8.0 sends both names with the same value. Prefer the
// family-neutral one; accept the legacy name so an older SPA or an existing
// script keeps working.
#define DV_AUTH_HEADER        "X-Dragon-Auth"
#define DV_AUTH_HEADER_LEGACY "X-DragonBreath-Auth"

static void ctl_token(char *out, size_t outsz)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(DV_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = outsz;
    nvs_get_str(h, DV_NVS_TOKEN, out, &sz);   // leaves out="" on any error
    nvs_close(h);
}

// Read whichever auth header is present into out. Returns false if neither is
// present, or the value is too long to be a valid token.
static bool auth_header(httpd_req_t *req, char *out, size_t outsz)
{
    const char *names[] = { DV_AUTH_HEADER, DV_AUTH_HEADER_LEGACY };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t len = httpd_req_get_hdr_value_len(req, names[i]);
        if (len == 0 || len >= outsz) continue;
        if (httpd_req_get_hdr_value_str(req, names[i], out, outsz) == ESP_OK && out[0])
            return true;
    }
    out[0] = '\0';
    return false;
}

static bool auth_ok(httpd_req_t *req)
{
    char token[DV_TOKEN_MAX + 1];
    ctl_token(token, sizeof token);
    char value[DV_TOKEN_MAX + 1] = {0};
    if (!auth_header(req, value, sizeof value)) return false;
    if (token[0]) return strcmp(value, token) == 0;   // configured -> exact match
    return true;                                      // else presence-only CSRF gate
}

// dc_portal calls this for its own routes (provisioning, logs, OTA, factory
// reset). DragonVent's product routes are registered directly with the server,
// so they are NOT covered by it — they gate themselves via auth_reject() below.
static bool authorize(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    return auth_ok(req);
}

// Reject a mutating request that fails auth with 403. Returns true if it did.
static bool auth_reject(httpd_req_t *req)
{
    if (auth_ok(req)) return false;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"ok\":false,\"error\":\"missing or invalid " DV_AUTH_HEADER " header\"}");
    return true;
}

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
    cJSON_AddBoolToObject(vent, "calibrating", dv_motor_is_calibrating());
    cJSON *cal = cJSON_AddArrayToObject(vent, "calibration");
    for (int i = 0; i < groups; ++i) {
        int o = -1, c = -1;
        dv_motor_calibration(i, &o, &c);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "open_mv", o);
        cJSON_AddNumberToObject(e, "closed_mv", c);
        cJSON_AddItemToArray(cal, e);
    }

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
    const char *values[] = { "vent_manual", "vent_auto", "vent_calibrate", "source_status", "polling", "provisioning" };
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
    if (auth_reject(req)) return ESP_OK;
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
    } else if (!strcmp(name->valuestring, "calibrate")) {
        err = dv_motor_recalibrate();   // sweeps open→closed, re-learns endstops
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
    if (auth_reject(req)) return ESP_OK;
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

static void add_rgb(cJSON *root, const char *key, const uint8_t c[3])
{
    cJSON *a = cJSON_AddArrayToObject(root, key);
    cJSON_AddItemToArray(a, cJSON_CreateNumber(c[0]));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(c[1]));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(c[2]));
}

static cJSON *lighting_json(void)
{
    dv_lighting_t c;
    dv_rgb_get_config(&c);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddBoolToObject(root, "enabled", c.enabled);
    cJSON_AddNumberToObject(root, "brightness", c.brightness);
    add_rgb(root, "open", c.open);
    add_rgb(root, "closed", c.closed);
    add_rgb(root, "printing", c.printing);
    cJSON_AddBoolToObject(root, "use_printing", c.use_printing);
    cJSON_AddBoolToObject(root, "use_temp", c.use_temp);
    cJSON_AddNumberToObject(root, "temp_min_c", c.temp_min_c);
    cJSON_AddNumberToObject(root, "temp_max_c", c.temp_max_c);
    cJSON_AddNumberToObject(root, "strips", dv_rgb_strip_count());
    return root;
}

static esp_err_t lighting_get(httpd_req_t *req) { return send_json(req, lighting_json()); }

// Parse an [r,g,b] array (0-255) into out, only if present + valid.
static void patch_rgb(cJSON *body, const char *key, uint8_t out[3])
{
    cJSON *a = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return;
    for (int i = 0; i < 3; ++i) {
        cJSON *e = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(e)) return;
    }
    for (int i = 0; i < 3; ++i) {
        int v = cJSON_GetArrayItem(a, i)->valueint;
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static esp_err_t lighting_post(httpd_req_t *req)
{
    if (auth_reject(req)) return ESP_OK;
    cJSON *body = recv_json(req);
    if (!body) return api_error(req, "400 Bad Request", "invalid json");

    dv_lighting_t c;
    dv_rgb_get_config(&c);   // start from current; patch only the fields present
    cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(body, "enabled")) && cJSON_IsBool(e)) c.enabled = cJSON_IsTrue(e);
    if ((e = cJSON_GetObjectItemCaseSensitive(body, "use_printing")) && cJSON_IsBool(e)) c.use_printing = cJSON_IsTrue(e);
    if ((e = cJSON_GetObjectItemCaseSensitive(body, "use_temp")) && cJSON_IsBool(e)) c.use_temp = cJSON_IsTrue(e);
    if ((e = cJSON_GetObjectItemCaseSensitive(body, "brightness")) && cJSON_IsNumber(e)) {
        int v = e->valueint; c.brightness = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(body, "temp_min_c")) && cJSON_IsNumber(e)) c.temp_min_c = (uint8_t)e->valueint;
    if ((e = cJSON_GetObjectItemCaseSensitive(body, "temp_max_c")) && cJSON_IsNumber(e)) c.temp_max_c = (uint8_t)e->valueint;
    patch_rgb(body, "open", c.open);
    patch_rgb(body, "closed", c.closed);
    patch_rgb(body, "printing", c.printing);
    cJSON_Delete(body);

    dv_rgb_set_config(&c);
    ++s_api_revision;
    return send_json(req, lighting_json());
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

// Reveal a section only when the control-source selector equals this value. Sections
// without it always render, so this stays backward-compatible with core < v0.6.1.
static void visible_when(cJSON *section, const char *field_key, const char *value)
{
    cJSON *vw = cJSON_AddObjectToObject(section, "visible_when");
    cJSON_AddStringToObject(vw, "field", field_key);
    cJSON_AddStringToObject(vw, "value", value);
}

// Refuse an OTA image that isn't ours. ESP-IDF's own verification (in esp_ota_end)
// already rejects corrupt images and the wrong chip — a DragonBreath image is
// ESP32-C3 and fails there — but any VALID ESP32 app image would otherwise be made
// the boot partition, leaving a device that boots into unrelated firmware. There is
// no published stock image to fall back to, so recovery would mean USB plus the
// owner's own flash backup. panda_vent is accepted so a stock image can be restored.
static esp_err_t validate_image(const esp_app_desc_t *image, void *ctx,
                                char *message, size_t message_size)
{
    (void)ctx;
    if (!strcmp(image->project_name, "dragonvent") || !strcmp(image->project_name, "panda_vent"))
        return ESP_OK;
    snprintf(message, message_size,
             "Not a DragonVent or stock Panda Vent image (got \"%s\").", image->project_name);
    return ESP_ERR_INVALID_ARG;
}

static cJSON *describe_product(void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_CreateObject(), *sections = cJSON_AddArrayToObject(root, "sections");

    // The selector lives alone in an always-visible section; the per-source
    // sections below reveal against it. The SPA looks the controlling field up
    // across the whole setup host, so it does not need to share their card.
    cJSON *printer = cJSON_CreateObject();
    cJSON_AddStringToObject(printer, "title", "Printer source");
    cJSON_AddStringToObject(printer, "description", "Choose one controller. Source changes take effect after restart.");
    cJSON *fields = cJSON_AddArrayToObject(printer, "fields");
    cJSON *source = field(fields, "source", "Control source", "select", dc_source_str(dc_source_get()));
    cJSON *options = cJSON_AddArrayToObject(source, "options");
    const char *source_values[][2] = {{"klipper","Klipper / Moonraker"},{"bambu","Bambu LAN"},{"none","Standalone"}};
    for (size_t i = 0; i < 3; ++i) { cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o,"value",source_values[i][0]); cJSON_AddStringToObject(o,"label",source_values[i][1]); cJSON_AddItemToArray(options,o); }
    cJSON_AddItemToArray(sections, printer);

    dc_moonraker_config_t mk = {0}; dc_moonraker_get_config(&mk);
    char port[8]; snprintf(port, sizeof(port), "%u", mk.port ?: 7125);
    cJSON *klipper = cJSON_CreateObject();
    cJSON_AddStringToObject(klipper, "title", "Klipper / Moonraker");
    visible_when(klipper, "source", "klipper");
    fields = cJSON_AddArrayToObject(klipper, "fields");
    field(fields, "moonraker_host", "Moonraker host", "text", mk.host);
    field(fields, "moonraker_port", "Moonraker port", "number", port);
    cJSON_AddBoolToObject(field(fields, "moonraker_api_key", "Moonraker API key", "text", ""), "secret", true);
    cJSON_AddItemToArray(sections, klipper);

    dc_bambu_config_t bb = {0}; dc_bambu_get_config(&bb);
    cJSON *bambu = cJSON_CreateObject();
    cJSON_AddStringToObject(bambu, "title", "Bambu LAN");
    visible_when(bambu, "source", "bambu");
    fields = cJSON_AddArrayToObject(bambu, "fields");
    field(fields, "bambu_host", "Bambu host", "text", bb.host);
    field(fields, "bambu_serial", "Bambu serial", "text", bb.serial);
    cJSON_AddBoolToObject(field(fields, "bambu_code", "Bambu access code", "text", ""), "secret", true);
    cJSON_AddItemToArray(sections, bambu);

    // Control token. Never echoed back — the field is always blank and only its
    // "configured" state is reported, so the secret cannot be read out of the
    // setup document. Blank on save leaves the current value alone; the explicit
    // "-" clears it (an empty string cannot mean both "unchanged" and "clear").
    char token[DV_TOKEN_MAX + 1];
    ctl_token(token, sizeof token);
    cJSON *security = cJSON_CreateObject();
    cJSON_AddStringToObject(security, "title", "Control token");
    cJSON_AddStringToObject(security, "description", token[0]
        ? "A token is set. Requests must send it. Leave blank to keep it, or enter - to remove it."
        : "No token set: any local request with the auth header is accepted. Set one to require it.");
    fields = cJSON_AddArrayToObject(security, "fields");
    cJSON_AddBoolToObject(field(fields, "control_token", "Control token", "text", ""), "secret", true);
    cJSON_AddItemToArray(sections, security);

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
    // Blank means "unchanged" (the field is never pre-filled, so every save would
    // otherwise clear it); a single "-" is the explicit clear.
    const char *token = string_value(values, "control_token");
    if (token && token[0]) {
        if (strlen(token) > DV_TOKEN_MAX) {
            snprintf(message, message_size, "Control token must be at most %d characters", DV_TOKEN_MAX);
            return ESP_ERR_INVALID_ARG;
        }
        nvs_handle_t h;
        if (nvs_open(DV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            snprintf(message, message_size, "Could not open storage");
            return ESP_FAIL;
        }
        esp_err_t err = strcmp(token, "-") == 0 ? nvs_erase_key(h, DV_NVS_TOKEN)
                                                : nvs_set_str(h, DV_NVS_TOKEN, token);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   // clearing an unset token
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err != ESP_OK) {
            snprintf(message, message_size, "Could not save the control token");
            return err;
        }
        dc_evlog_add("control token %s", strcmp(token, "-") == 0 ? "cleared" : "set");
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
        { .uri = "/api/v2/lighting", .method = HTTP_GET, .handler = lighting_get },
        { .uri = "/api/v2/lighting", .method = HTTP_POST, .handler = lighting_post },
    };
    const dc_portal_config_t config = {
        .product = "dragonvent", .display_name = "DragonVent",
        .product_routes = routes, .product_route_count = sizeof(routes) / sizeof(routes[0]),
        .describe_product = describe_product, .apply_product = apply_product,
        .authorize = authorize,
        .validate_image = validate_image,
        .factory_reset = factory_reset,
    };
    return dc_portal_start(&config);
}

esp_err_t dv_portal_stop(void) { return dc_portal_stop(); }
