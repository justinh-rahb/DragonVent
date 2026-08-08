#include "dv_portal.h"
#include "dv_dns.h"
#include "dc_bambu.h"
#include "dc_evlog.h"
#include "dc_moonraker.h"
#include "dc_source.h"
#include "dv_policy.h"
#include "dc_wifi.h"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"

#include <math.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "dv_portal";

static httpd_handle_t s_httpd = NULL;
static bool           s_ap_mode = false;

// ---------- URL-encoded form parsing ----------

// Extract a value for `key` from a url-encoded form body. NUL-terminates.
// Returns 0 on success, -1 if not found.
static int form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < out_sz) {
                char c = *p++;
                if (c == '+') {
                    out[o++] = ' ';
                } else if (c == '%' && p[0] && p[1]) {
                    char hex[3] = { p[0], p[1], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 2;
                } else {
                    out[o++] = c;
                }
            }
            out[o] = '\0';
            return 0;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return -1;
}

// Copy a value into a buffer with HTML-attribute-safe escaping.
static void html_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < out_sz; ++p) {
        const char *r = NULL;
        switch (*p) {
            case '&':  r = "&amp;";  break;
            case '<':  r = "&lt;";   break;
            case '>':  r = "&gt;";   break;
            case '"':  r = "&quot;"; break;
            case '\'': r = "&#39;";  break;
        }
        if (r) {
            size_t rl = strlen(r);
            memcpy(out + o, r, rl);
            o += rl;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

// Read the request body into `out`.
static int recv_body(httpd_req_t *req, char *out, size_t out_sz)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)out_sz) return -1;
    int off = 0;
    while (off < total) {
        int n = httpd_req_recv(req, out + off, total - off);
        if (n <= 0) return -1;
        off += n;
    }
    out[off] = '\0';
    return off;
}

// Parse a dotted-quad IPv4 string into host-byte-order uint32. Returns 0 on
// failure.
static uint32_t parse_ipv4(const char *s)
{
    if (s == NULL || *s == '\0') return 0;
    struct in_addr a;
    if (inet_aton(s, &a) == 0) return 0;
    return ntohl(a.s_addr);
}

// ---------- status/label helpers ----------

static const char *wifi_label(dc_wifi_state_t s)
{
    switch (s) {
        case DC_WIFI_STATE_INIT:            return "starting";
        case DC_WIFI_STATE_STA_CONNECTING:  return "connecting";
        case DC_WIFI_STATE_STA_CONNECTED:   return "STA connected";
        case DC_WIFI_STATE_AP_PORTAL:       return "AP mode (setup)";
    }
    return "?";
}

static const char *mk_label(dc_moonraker_state_t s)
{
    switch (s) {
        case DC_MK_DISABLED:      return "not configured";
        case DC_MK_DISCONNECTED:  return "disconnected";
        case DC_MK_CONNECTING:    return "connecting…";
        case DC_MK_CONNECTED:     return "handshaking";
        case DC_MK_SUBSCRIBED:    return "connected";
    }
    return "?";
}

static const char *bambu_label(dc_bambu_state_t s)
{
    switch (s) {
        case DC_BAMBU_DISABLED:     return "not configured";
        case DC_BAMBU_DISCONNECTED: return "disconnected";
        case DC_BAMBU_CONNECTING:   return "connecting…";
        case DC_BAMBU_CONNECTED:    return "handshaking";
        case DC_BAMBU_SUBSCRIBED:   return "connected";
    }
    return "?";
}

static const char *target_label(dv_motor_target_t t)
{
    return t == DV_MOTOR_TARGET_OPEN ? "OPEN"
         : t == DV_MOTOR_TARGET_CLOSED ? "CLOSED" : "STOP";
}

// ---------- HTML rendering (chunked) ----------

#define SEND(req, buf) httpd_resp_send_chunk((req), (buf), HTTPD_RESP_USE_STRLEN)

static esp_err_t send_conflict(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t send_head(httpd_req_t *req)
{
    static const char *HEAD =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>DragonVent</title>"
"<style>"
":root{--code-bg:#f4f4f4;--code-fg:#222}"
"body{font-family:sans-serif;max-width:520px;margin:1em auto;padding:0 1em;color:#222}"
"h1{font-size:1.3em;margin-bottom:.2em}"
"h2{font-size:1.05em;margin-top:1.6em;border-bottom:1px solid #ddd;padding-bottom:.2em}"
"label{display:block;margin:.7em 0 .2em;font-size:.9em;color:#555}"
"input,button,select{width:100%;padding:.55em;font-size:1em;box-sizing:border-box}"
"button{margin-top:1em;background:#222;color:#fff;border:0;border-radius:4px;cursor:pointer}"
"button.secondary{background:#eee;color:#222;margin-top:.5em}"
".status{background:#f4f4f4;padding:.7em 1em;border-radius:4px;font-size:.9em;line-height:1.4}"
".status b{display:inline-block;min-width:110px}"
".row{display:flex;gap:.5em}.row>*{flex:1}"
".radios label{display:inline-block;margin-right:1em;font-size:1em;color:#222}"
".radios input{width:auto;margin-right:.3em}"
".hint{color:#666;font-size:.85em;margin-top:.2em}"
"code,pre{background:var(--code-bg);color:var(--code-fg);border-radius:3px}"
"code{padding:.1em .3em}"
"pre{padding:.6em;overflow-x:auto;font-size:.9em;margin:.4em 0}"
// Show-password toggle used on the WiFi and AP-config password fields.
// Wraps input+button in a flex row so the eye button hugs the right edge.
".pwd{display:flex;gap:.4em}.pwd input{flex:1}"
".pwd button{width:auto;padding:.55em .9em;margin-top:0;background:#eee;color:#222}"
// Tab nav. CSS-only: sections default hidden, :target shows one, #home is
// shown by default and hidden if any earlier sibling is :target.
"nav.tabs{display:flex;gap:.2em;border-bottom:1px solid #ddd;margin:1em 0}"
"nav.tabs a{padding:.6em 1em;text-decoration:none;color:#666;border-radius:4px 4px 0 0}"
"nav.tabs a:hover{background:#f4f4f4;color:#222}"
".tab{display:none}.tab:target{display:block}"
"#home{display:block}.tab:target~#home{display:none}"
// Dark mode. Follows the browser / OS preference — no toggle in the UI, so
// nothing extra for the user to persist. Everything's on --prefixed CSS
// variables to keep the light-mode rules above untouched.
"@media(prefers-color-scheme:dark){"
  ":root{--code-bg:#1c1c1c;--code-fg:#e6e6e6}"
  "body{background:#111;color:#e6e6e6}"
  "h2{border-bottom-color:#333}"
  "label,.radios label{color:#aaa}"
  ".hint{color:#888}"
  "input,select{background:#1c1c1c;color:#e6e6e6;border:1px solid #444}"
  "button{background:#444;color:#fff}"
  "button.secondary{background:#2a2a2a;color:#e6e6e6}"
  ".status{background:#1c1c1c}"
  ".pwd button{background:#2a2a2a;color:#e6e6e6}"
  "nav.tabs{border-bottom-color:#333}"
  "nav.tabs a{color:#aaa}"
  "nav.tabs a:hover{background:#1c1c1c;color:#fff}"
"}"
"</style></head><body>"
"<h1>DragonVent</h1>"
"<nav class=\"tabs\">"
  "<a href=\"#home\">Home</a>"
  "<a href=\"#wifi\">WiFi</a>"
  "<a href=\"#printer\">Printer</a>"
  "<a href=\"#log\">Log</a>"
  "<a href=\"#system\">System</a>"
"</nav>";
    return SEND(req, HEAD);
}

// Extra network detail — STA IP if connected, RSSI, AP IP if serving. Blank
// strings if not applicable.
static void gather_wifi_detail(char *wifi_line, size_t wifi_sz)
{
    dc_wifi_state_t st = dc_wifi_state();
    if (st == DC_WIFI_STATE_STA_CONNECTED) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {0};
        if (sta) esp_netif_get_ip_info(sta, &info);
        wifi_ap_record_t ap;
        int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        snprintf(wifi_line, wifi_sz, "connected · " IPSTR " · %d dBm",
                 IP2STR(&info.ip), rssi);
    } else if (st == DC_WIFI_STATE_AP_PORTAL) {
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t info = {0};
        if (ap) esp_netif_get_ip_info(ap, &info);
        snprintf(wifi_line, wifi_sz, "AP mode · " IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(wifi_line, wifi_sz, "%s", wifi_label(st));
    }
}

static esp_err_t send_status(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    dc_ctl_source_t source = dc_source_get();

    char wifi_line[96];
    gather_wifi_detail(wifi_line, sizeof(wifi_line));

    char connection_line[96] = "standalone";
    char printer_line[128] = "not bound";
    char material_line[64] = "";
    char chamber_line[64] = "";

    if (source == DC_SRC_KLIPPER) {
        dc_moonraker_status_t mk = {0};
        dc_moonraker_get_status(&mk);
        snprintf(connection_line, sizeof(connection_line), "Moonraker · %s", mk_label(mk.state));
        snprintf(printer_line, sizeof(printer_line), "%s (bed %.1f\xC2\xB0""C)",
                 dc_printer_state_str(mk.printer), mk.bed_temp);
        if (mk.material[0]) snprintf(material_line, sizeof(material_line),
                                     "<div><b>Material:</b> %s</div>", mk.material);
        if (!isnan(mk.chamber_temp)) snprintf(chamber_line, sizeof(chamber_line),
                                               "<div><b>Chamber:</b> %.1f\xC2\xB0""C</div>",
                                               mk.chamber_temp);
    } else if (source == DC_SRC_BAMBU) {
        dc_bambu_status_t bb = {0};
        dc_bambu_get_status(&bb);
        snprintf(connection_line, sizeof(connection_line), "Bambu LAN · %s", bambu_label(bb.state));
        snprintf(printer_line, sizeof(printer_line), "%s (bed %.1f\xC2\xB0""C)",
                 bb.printing ? "printing" : "idle", bb.bed_temp);
        if (bb.filament[0]) snprintf(material_line, sizeof(material_line),
                                     "<div><b>Material:</b> %s</div>", bb.filament);
        if (!isnan(bb.chamber_temp)) snprintf(chamber_line, sizeof(chamber_line),
                                               "<div><b>Chamber:</b> %.1f\xC2\xB0""C</div>",
                                               bb.chamber_temp);
    }

    char buf[1200];
    snprintf(buf, sizeof(buf),
        "<div class=\"status\">"
        "<div><b>Firmware:</b> %s</div>"
        "<div><b>WiFi:</b> %s</div>"
        "<div><b>Source:</b> %s</div>"
        "<div><b>Connection:</b> %s</div>"
        "<div><b>Printer:</b> %s</div>"
        "%s%s"
        "<div><b>Vent target:</b> %s</div>"
        "<div><b>Mode:</b> %s</div>"
        "</div>",
        app->version,
        wifi_line,
        dc_source_str(source),
        connection_line,
        printer_line,
        material_line,
        chamber_line,
        target_label(dv_policy_get_target()),
        dv_policy_get_mode() == DV_POLICY_MODE_MANUAL ? "MANUAL" : "AUTO");
    return SEND(req, buf);
}

static esp_err_t send_source_section(httpd_req_t *req)
{
    dc_ctl_source_t source = dc_source_get();
    char buf[1400];
    snprintf(buf, sizeof(buf),
        "<h2>Printer source</h2>"
        "<form method=\"POST\" action=\"/source\">"
        "<div class=\"hint\">DragonVent connects to exactly one printer source. Changing it reboots the device; configure the selected source after it returns.</div>"
        "<div class=\"radios\">"
          "<label><input type=\"radio\" name=\"source\" value=\"klipper\"%s>Klipper / Moonraker</label>"
          "<label><input type=\"radio\" name=\"source\" value=\"bambu\"%s>Bambu LAN</label>"
          "<label><input type=\"radio\" name=\"source\" value=\"none\"%s>Standalone</label>"
        "</div>"
        "<button>Save source &amp; reboot</button>"
        "</form>",
        source == DC_SRC_KLIPPER ? " checked" : "",
        source == DC_SRC_BAMBU ? " checked" : "",
        source == DC_SRC_NONE ? " checked" : "");
    return SEND(req, buf);
}

static esp_err_t send_wifi_section(httpd_req_t *req)
{
    // Header + save form open.
    SEND(req,
        "<h2>WiFi</h2>"
        "<form method=\"POST\" action=\"/wifi\">"
        "<label>Network</label>"
        "<select name=\"ssid\">"
        "<option value=\"\">— pick a network —</option>");

    // Scanned networks. `recs` is static so we don't put ~1.6 KB on the
    // httpd task's 4 KB stack; httpd is single-threaded, so no lock needed.
    static wifi_ap_record_t recs[DC_WIFI_SCAN_MAX];
    int n = dc_wifi_get_scan_results(recs, DC_WIFI_SCAN_MAX);
    for (int i = 0; i < n; ++i) {
        char ssid_esc[80];
        html_escape((const char *)recs[i].ssid, ssid_esc, sizeof(ssid_esc));
        // Two escaped SSIDs + fixed template, worst case ~200 bytes.
        char row[256];
        snprintf(row, sizeof(row),
            "<option value=\"%s\">%s (%d dBm%s)</option>",
            ssid_esc, ssid_esc,
            recs[i].rssi,
            recs[i].authmode == WIFI_AUTH_OPEN ? ", open" : "");
        SEND(req, row);
    }

    // Manual entry + password + save. Scan status hint.
    char tail[900];
    const char *scan_hint = dc_wifi_is_scanning()
        ? "<div class=\"hint\">Scanning…</div>"
        : (n == 0
            ? "<div class=\"hint\">No networks cached — click Scan.</div>"
            : "");
    snprintf(tail, sizeof(tail),
        "</select>"
        "%s"
        "<label>Or enter SSID manually (used if dropdown left blank)</label>"
        "<input name=\"ssid_manual\" maxlength=\"32\" autocomplete=\"off\">"
        "<label>Password</label>"
        "<div class=\"pwd\">"
          "<input name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
          "<button type=\"button\" onclick=\"var i=this.previousElementSibling;"
            "var s=i.type=='password';i.type=s?'text':'password';"
            "this.textContent=s?'Hide':'Show'\">Show</button>"
        "</div>"
        "<button>Save WiFi &amp; reboot</button>"
        "</form>"
        "<form method=\"POST\" action=\"/scan\">"
        "<button class=\"secondary\">Rescan networks</button>"
        "</form>",
        scan_hint);
    return SEND(req, tail);
}

static esp_err_t send_moonraker_section(httpd_req_t *req)
{
    dc_moonraker_config_t mk_cfg = {0};
    dc_moonraker_get_config(&mk_cfg);
    char host_esc[128];
    html_escape(mk_cfg.host, host_esc, sizeof(host_esc));

    char buf[800];
    snprintf(buf, sizeof(buf),
        "<h2>Moonraker</h2>"
        "<form method=\"POST\" action=\"/moonraker\">"
        "<div class=\"row\">"
          "<div><label>Host / IP</label><input name=\"host\" value=\"%s\" required maxlength=\"63\"></div>"
          "<div style=\"max-width:130px\"><label>Port</label><input name=\"port\" type=\"number\" value=\"%u\" min=\"1\" max=\"65535\"></div>"
        "</div>"
        "<label>API key (leave blank to keep current)</label>"
        "<input name=\"api_key\" maxlength=\"64\" autocomplete=\"off\">"
        "<button>Save Moonraker</button>"
        "</form>",
        host_esc, mk_cfg.port ? mk_cfg.port : 7125);
    return SEND(req, buf);
}

static esp_err_t send_bambu_section(httpd_req_t *req)
{
    dc_bambu_config_t cfg = {0};
    dc_bambu_get_config(&cfg);
    char host_esc[128], serial_esc[80];
    html_escape(cfg.host, host_esc, sizeof(host_esc));
    html_escape(cfg.serial, serial_esc, sizeof(serial_esc));

    char buf[1100];
    snprintf(buf, sizeof(buf),
        "<h2>Bambu LAN</h2>"
        "<form method=\"POST\" action=\"/bambu\">"
        "<div class=\"hint\">Read-only LAN MQTT. DragonVent reads printer state and never sends printer-control commands.</div>"
        "<label>Printer IP / host</label><input name=\"host\" value=\"%s\" required maxlength=\"63\">"
        "<label>Printer serial</label><input name=\"serial\" value=\"%s\" required maxlength=\"31\">"
        "<label>LAN access code (leave blank to keep current)</label>"
        "<div class=\"pwd\">"
          "<input name=\"code\" type=\"password\" maxlength=\"31\" autocomplete=\"off\">"
          "<button type=\"button\" onclick=\"var i=this.previousElementSibling;var s=i.type=='password';i.type=s?'text':'password';this.textContent=s?'Hide':'Show'\">Show</button>"
        "</div>"
        "<button>Save Bambu &amp; reboot</button>"
        "</form>", host_esc, serial_esc);
    return SEND(req, buf);
}

static esp_err_t send_policy_section(httpd_req_t *req)
{
    float open_c = 45.0f, close_c = 35.0f;
    dv_policy_get_thresholds(&open_c, &close_c);

    char buf[900];
    snprintf(buf, sizeof(buf),
        "<h2>Auto-mode thresholds</h2>"
        "<form method=\"POST\" action=\"/policy\">"
        "<div class=\"hint\">"
          "When the printer is idle or a print has completed, the vent opens once the bed climbs above the OPEN threshold and closes once it drops below the CLOSE threshold. During a print, the material rule (PLA opens / ABS seals) takes over."
        "</div>"
        "<div class=\"row\">"
          "<div><label>Bed OPEN above (\xC2\xB0""C)</label>"
            "<input name=\"bed_open\"  type=\"number\" step=\"0.5\" min=\"20\" max=\"120\" value=\"%.1f\" required></div>"
          "<div><label>Bed CLOSE below (\xC2\xB0""C)</label>"
            "<input name=\"bed_close\" type=\"number\" step=\"0.5\" min=\"10\" max=\"120\" value=\"%.1f\" required></div>"
        "</div>"
        "<div class=\"hint\">OPEN must be strictly greater than CLOSE — the gap is the hysteresis band.</div>"
        "<button>Save thresholds</button>"
        "</form>",
        open_c, close_c);
    esp_err_t r = SEND(req, buf);
    if (r != ESP_OK) return r;

    // Material rules — informational for 0.3.0. Editable overrides land later.
    return SEND(req,
        "<h2>Material rules</h2>"
        "<div class=\"hint\">"
          "When your printer reports a material, the vent decides based on the family. "
          "Otherwise it falls back to the bed-temperature rules above."
        "</div>"
        "<table style=\"width:100%;border-collapse:collapse;margin-top:.5em\">"
        "<tr><th style=\"text-align:left;padding:.3em .5em\">Material</th>"
            "<th style=\"text-align:left;padding:.3em .5em\">Behavior during print</th></tr>"
        "<tr><td style=\"padding:.3em .5em\">PLA, PETG, PET, TPU</td>"
            "<td style=\"padding:.3em .5em\">Open — vent for cooling</td></tr>"
        "<tr><td style=\"padding:.3em .5em\">ABS, ASA, PC, PA (nylon), HIPS</td>"
            "<td style=\"padding:.3em .5em\">Sealed — retain heat</td></tr>"
        "<tr><td style=\"padding:.3em .5em\">Unknown / not reported</td>"
            "<td style=\"padding:.3em .5em\">Open (safe default for hobby prints)</td></tr>"
        "</table>"
        "<div class=\"hint\" style=\"margin-top:.75em\">"
          "<b>To report material to DragonVent</b>, add this to your Klipper "
          "<code>PRINT_START</code> macro so it fires at the start of every print:"
        "</div>"
        "<pre>"
          "SAVE_VARIABLE VARIABLE=material VALUE='\"{material}\"'"
        "</pre>"
        "<div class=\"hint\">"
          "Most slicers (SuperSlicer, PrusaSlicer, OrcaSlicer) expose "
          "<code>{material}</code> or <code>{filament_type[0]}</code> in the start-gcode "
          "template. Custom per-material overrides are on the roadmap."
        "</div>");
}

static esp_err_t send_mode_section(httpd_req_t *req)
{
    bool manual = dv_policy_get_mode() == DV_POLICY_MODE_MANUAL;
    dv_motor_target_t target = dv_policy_get_target();
    char buf[1200];
    snprintf(buf, sizeof(buf),
        "<h2>Mode</h2>"
        // Quick-action buttons: each posts to /vent with a hidden target.
        // Same effect as short-pressing the physical button — switches to
        // MANUAL and drives to that state.
        "<label>Quick action</label>"
        "<div class=\"row\">"
          "<form method=\"POST\" action=\"/vent\" style=\"flex:1\">"
            "<input type=\"hidden\" name=\"target\" value=\"open\">"
            "<button>Open vent</button>"
          "</form>"
          "<form method=\"POST\" action=\"/vent\" style=\"flex:1\">"
            "<input type=\"hidden\" name=\"target\" value=\"closed\">"
            "<button>Close vent</button>"
          "</form>"
        "</div>"

        "<form method=\"POST\" action=\"/mode\" style=\"margin-top:1em\">"
        "<div class=\"radios\">"
          "<label><input type=\"radio\" name=\"mode\" value=\"auto\"%s>Auto</label>"
          "<label><input type=\"radio\" name=\"mode\" value=\"manual\"%s>Manual</label>"
        "</div>"
        "<label style=\"margin-top:1em\">Manual target (used in Manual mode)</label>"
        "<div class=\"radios\">"
          "<label><input type=\"radio\" name=\"manual_target\" value=\"open\"%s>Open</label>"
          "<label><input type=\"radio\" name=\"manual_target\" value=\"closed\"%s>Closed</label>"
        "</div>"
        "<button>Save Mode</button>"
        "</form>",
        manual ? "" : " checked",
        manual ? " checked" : "",
        target == DV_MOTOR_TARGET_OPEN ? " checked" : "",
        target != DV_MOTOR_TARGET_OPEN ? " checked" : "");
    return SEND(req, buf);
}

static esp_err_t send_ap_section(httpd_req_t *req)
{
    dc_wifi_ap_config_t ap = {0};
    dc_wifi_get_ap_config(&ap);
    char ssid_esc[80], pass_esc[128];
    html_escape(ap.ssid,     ssid_esc, sizeof(ssid_esc));
    html_escape(ap.password, pass_esc, sizeof(pass_esc));

    char buf[1600];
    snprintf(buf, sizeof(buf),
        "<h2>AP Hotspot</h2>"
        "<form method=\"POST\" action=\"/ap_config\">"
        "<div class=\"hint\">Used when there's no saved WiFi or the saved network is unreachable. Saving reboots.</div>"
        "<label style=\"margin-top:1em\">"
          "<input type=\"checkbox\" name=\"ap_enabled\" value=\"1\" style=\"width:auto;margin-right:.5em\"%s>"
          "Allow AP fallback when WiFi is down"
        "</label>"
        "<div class=\"hint\">If unchecked, the device won't expose an AP even when it can't reach your WiFi. If you lose WiFi you'll need serial access or a BOOT-button factory reset to recover.</div>"
        "<label>SSID</label><input name=\"ap_ssid\" value=\"%s\" maxlength=\"32\">"
        "<label>Password (blank = keep, min 8 chars for WPA2)</label>"
        "<div class=\"pwd\">"
          "<input name=\"ap_password\" type=\"password\" value=\"%s\" maxlength=\"63\">"
          "<button type=\"button\" onclick=\"var i=this.previousElementSibling;"
            "var s=i.type=='password';i.type=s?'text':'password';"
            "this.textContent=s?'Hide':'Show'\">Show</button>"
        "</div>"
        "<label>IP address</label>"
        "<input name=\"ap_ip\" value=\"%u.%u.%u.%u\" maxlength=\"15\">"
        "<button>Save AP &amp; reboot</button>"
        "</form>",
        ap.enabled ? " checked" : "",
        ssid_esc, pass_esc,
        (unsigned)((ap.ip >> 24) & 0xFF),
        (unsigned)((ap.ip >> 16) & 0xFF),
        (unsigned)((ap.ip >>  8) & 0xFF),
        (unsigned)( ap.ip        & 0xFF));
    return SEND(req, buf);
}

static esp_err_t send_log_section(httpd_req_t *req)
{
    esp_err_t r = SEND(req,
        "<h2>Event log</h2>"
        "<div class=\"hint\">"
          "Newest events first. Ring buffer resets on reboot. Timestamp is "
          "seconds since boot."
        "</div>"
        "<table style=\"width:100%;border-collapse:collapse;font-size:.9em;font-family:monospace\">"
        "<tr><th style=\"text-align:right;padding:.2em .5em;width:6em\">t (s)</th>"
            "<th style=\"text-align:left;padding:.2em .5em\">event</th></tr>");
    if (r != ESP_OK) return r;

    // Static so we don't put ~6 KB on the httpd task stack. httpd is
    // single-threaded so no lock is needed here.
    static dc_evlog_entry_t entries[DC_EVLOG_MAX_ENTRIES];
    size_t n = dc_evlog_snapshot(entries, DC_EVLOG_MAX_ENTRIES);
    if (n == 0) {
        r = SEND(req, "<tr><td colspan=\"2\" style=\"padding:.4em .5em;color:#888\">"
                      "No events yet.</td></tr>");
        if (r != ESP_OK) return r;
    } else {
        for (size_t i = 0; i < n; ++i) {
            char text_esc[DC_EVLOG_TEXT_BYTES * 6];
            html_escape(entries[i].text, text_esc, sizeof(text_esc));

            char row_open[160];
            snprintf(row_open, sizeof(row_open),
                "<tr><td style=\"text-align:right;padding:.2em .5em;color:#666\">%lu.%03lu</td>"
                "<td style=\"padding:.2em .5em\">",
                (unsigned long)(entries[i].ms / 1000),
                (unsigned long)(entries[i].ms % 1000));
            r = SEND(req, row_open);
            if (r != ESP_OK) return r;

            r = SEND(req, text_esc);
            if (r != ESP_OK) return r;

            r = SEND(req, "</td></tr>");
            if (r != ESP_OK) return r;
        }
    }
    return SEND(req, "</table>");
}

static esp_err_t send_danger_section(httpd_req_t *req)
{
    return SEND(req,
        "<h2 style=\"color:#a33\">Danger zone</h2>"
        "<form method=\"POST\" action=\"/factory_reset\""
        " onsubmit=\"return confirm('Wipe all saved settings and reboot?');\">"
        "<div class=\"hint\">Clears WiFi, Moonraker, and mode config from NVS, then reboots. Same as holding the BOOT button on the module for 3 seconds.</div>"
        "<button style=\"background:#a33\">Factory reset</button>"
        "</form>");
}

// OTA upload uses a tiny JS shim so we can POST the file as
// application/octet-stream and skip multipart parsing on the ESP side. Works
// without JS enabled the browser will just show the form and the button will
// do nothing — acceptable for a modern config UI.
static esp_err_t send_ota_section(httpd_req_t *req)
{
    return SEND(req,
        "<h2>OTA firmware update</h2>"
        "<div class=\"hint\">Upload a <code>dragonvent-ota.bin</code> from the releases page. The full-flash image (<code>-full.bin</code>) is only for esptool — don't upload it here.</div>"
        "<form id=\"ota\" onsubmit=\"return uploadOta(event)\">"
          "<label>Firmware file</label>"
          "<input type=\"file\" id=\"otafile\" accept=\".bin\" required>"
          "<button>Upload &amp; flash</button>"
        "</form>"
        "<div id=\"otaStatus\" class=\"hint\"></div>"
        "<script>"
        "async function uploadOta(e){"
          "e.preventDefault();"
          "const f=document.getElementById('otafile').files[0];"
          "const s=document.getElementById('otaStatus');"
          "if(!f)return false;"
          "s.textContent='Uploading '+f.name+' ('+f.size+' bytes)…';"
          "try{"
            "const r=await fetch('/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
            "if(r.ok){s.innerHTML='<b>Success — rebooting.</b>'}"
            "else{s.textContent='Failed: '+await r.text()}"
          "}catch(err){s.textContent='Failed: '+err.message}"
          "return false;"
        "}"
        "</script>");
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    send_head(req);

    // Order matters: for the CSS `.tab:target ~ #home` trick to hide #home
    // when another tab is selected, #home has to appear LAST among the tabs
    // in DOM order (adjacent-sibling selector only reaches later siblings).

    SEND(req, "<section id=\"wifi\" class=\"tab\">");
    send_wifi_section(req);
    send_ap_section(req);
    SEND(req, "</section>");

    SEND(req, "<section id=\"printer\" class=\"tab\">");
    send_source_section(req);
    dc_ctl_source_t source = dc_source_get();
    if (source == DC_SRC_KLIPPER) send_moonraker_section(req);
    else if (source == DC_SRC_BAMBU) send_bambu_section(req);
    send_policy_section(req);
    SEND(req, "</section>");

    SEND(req, "<section id=\"log\" class=\"tab\">");
    send_log_section(req);
    SEND(req, "</section>");

    SEND(req, "<section id=\"system\" class=\"tab\">");
    send_ota_section(req);
    send_danger_section(req);
    SEND(req, "</section>");

    SEND(req, "<section id=\"home\" class=\"tab\">");
    send_status(req);
    send_mode_section(req);
    SEND(req, "</section>");

    SEND(req, "</body></html>");
    httpd_resp_send_chunk(req, NULL, 0);   // terminate chunked stream
    return ESP_OK;
}

// ---------- POST handlers ----------

static esp_err_t handle_wifi_post(httpd_req_t *req)
{
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char ssid[33] = {0}, ssid_manual[33] = {0}, pass[65] = {0};
    form_get(body, "ssid",        ssid,        sizeof(ssid));
    form_get(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    form_get(body, "password",    pass,        sizeof(pass));

    // Prefer the dropdown selection; fall back to manual entry for hidden APs.
    const char *chosen = ssid[0] ? ssid : ssid_manual;
    if (chosen[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SSID chosen");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><body><h1>Saved. Rebooting…</h1></body>");
    dc_wifi_save_creds_and_reboot(chosen, pass);   // does not return
    return ESP_OK;
}

static esp_err_t handle_scan_post(httpd_req_t *req)
{
    esp_err_t err = dc_wifi_scan_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
    }
    // Small delay so the caller who reloads has a better chance of seeing
    // results without an extra manual refresh.
    vTaskDelay(pdMS_TO_TICKS(2500));
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/#wifi");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_source_post(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char value[16] = {0};
    if (form_get(body, "source", value, sizeof(value)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing source");
        return ESP_OK;
    }

    dc_ctl_source_t source;
    if (strcmp(value, "klipper") == 0) source = DC_SRC_KLIPPER;
    else if (strcmp(value, "bambu") == 0) source = DC_SRC_BAMBU;
    else if (strcmp(value, "none") == 0) source = DC_SRC_NONE;
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid source");
        return ESP_OK;
    }
    esp_err_t err = dc_source_set(source);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!DOCTYPE html><body><h1>Source saved. Rebooting…</h1></body>");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_moonraker_post(httpd_req_t *req)
{
    if (dc_source_get() != DC_SRC_KLIPPER) {
        return send_conflict(req, "Klipper source is not selected");
    }
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    dc_moonraker_config_t cfg = {0};
    dc_moonraker_get_config(&cfg);   // start from current (preserves api_key on blank)

    char host[64] = {0}, port_str[8] = {0}, api_key[65] = {0};
    if (form_get(body, "host", host, sizeof(host)) != 0 || host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing host");
        return ESP_OK;
    }
    strncpy(cfg.host, host, sizeof(cfg.host) - 1);

    if (form_get(body, "port", port_str, sizeof(port_str)) == 0 && port_str[0]) {
        long p = strtol(port_str, NULL, 10);
        if (p > 0 && p < 65536) cfg.port = (uint16_t)p;
    }
    if (form_get(body, "api_key", api_key, sizeof(api_key)) == 0 && api_key[0] != '\0') {
        strncpy(cfg.api_key, api_key, sizeof(cfg.api_key) - 1);
    }
    esp_err_t err = dc_moonraker_set_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "moonraker_set_config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/#printer");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_bambu_post(httpd_req_t *req)
{
    if (dc_source_get() != DC_SRC_BAMBU) {
        return send_conflict(req, "Bambu source is not selected");
    }
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    dc_bambu_config_t cfg = {0};
    dc_bambu_get_config(&cfg);
    char host[64] = {0}, serial[32] = {0}, code[32] = {0};
    if (form_get(body, "host", host, sizeof(host)) != 0 || host[0] == '\0' ||
        form_get(body, "serial", serial, sizeof(serial)) != 0 || serial[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "host and serial are required");
        return ESP_OK;
    }
    snprintf(cfg.host, sizeof(cfg.host), "%s", host);
    snprintf(cfg.serial, sizeof(cfg.serial), "%s", serial);
    if (form_get(body, "code", code, sizeof(code)) == 0 && code[0] != '\0') {
        snprintf(cfg.code, sizeof(cfg.code), "%s", code);
    }
    esp_err_t err = dc_bambu_set_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!DOCTYPE html><body><h1>Bambu saved. Rebooting…</h1></body>");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_policy_post(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char open_s[16] = {0}, close_s[16] = {0};
    form_get(body, "bed_open",  open_s,  sizeof(open_s));
    form_get(body, "bed_close", close_s, sizeof(close_s));
    if (open_s[0] == '\0' || close_s[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing thresholds");
        return ESP_OK;
    }
    float open_c  = strtof(open_s,  NULL);
    float close_c = strtof(close_s, NULL);
    esp_err_t err = dv_policy_set_thresholds(open_c, close_c);
    if (err != ESP_OK) {
        // Almost certainly the OPEN <= CLOSE guard tripped.
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "OPEN must be strictly greater than CLOSE");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/#printer");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_mode_post(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char mode[16] = {0}, manual_target[16] = {0};
    form_get(body, "mode", mode, sizeof(mode));
    form_get(body, "manual_target", manual_target, sizeof(manual_target));

    if (strcmp(mode, "manual") == 0) {
        dv_motor_target_t t = strcmp(manual_target, "open") == 0
                                  ? DV_MOTOR_TARGET_OPEN
                                  : DV_MOTOR_TARGET_CLOSED;
        dv_policy_set_manual_target(t);
        dv_policy_set_mode(DV_POLICY_MODE_MANUAL);
    } else {
        dv_policy_set_mode(DV_POLICY_MODE_AUTO);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/#home");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_vent_post(httpd_req_t *req)
{
    char body[64];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char target[16] = {0};
    if (form_get(body, "target", target, sizeof(target)) != 0 || target[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing target");
        return ESP_OK;
    }
    dv_motor_target_t t = strcmp(target, "open") == 0 ? DV_MOTOR_TARGET_OPEN
                                                     : DV_MOTOR_TARGET_CLOSED;
    // Same semantics as the physical button short-press: force MANUAL and
    // drive to the chosen state.
    dv_policy_set_manual_target(t);
    dv_policy_set_mode(DV_POLICY_MODE_MANUAL);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/#home");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_ota_post(httpd_req_t *req)
{
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (upd == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no OTA partition available");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA start (target=%s, size=%d)", upd->label, req->content_len);

    esp_ota_handle_t update = 0;
    esp_err_t err = esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }

    // Static so we don't put 1 KB on the httpd task's stack.
    static char rx[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, rx, sizeof(rx));
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "recv failed at %d bytes remaining", remaining);
            esp_ota_abort(update);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        err = esp_ota_write(update, rx, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(update);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                esp_err_to_name(err));
            return ESP_OK;
        }
        remaining -= n;
    }

    err = esp_ota_end(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    err = esp_ota_set_boot_partition(upd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA success — rebooting into %s", upd->label);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}

// Tasmota-compatible power endpoint. Lets users control the vent from Klipper
// gcode by adding this to moonraker.conf:
//
//     [power vent]
//     type: tasmota
//     address: DragonVent.local
//
// then calling `POWER_ON vent` / `POWER_OFF vent` from a macro. Also gives
// them a toggle in Mainsail/Fluidd's Power panel for free.
//
// Semantics match a portal quick-action button: force MANUAL and drive to
// the requested state. POWER_OFF does NOT return to AUTO — the user opts
// back in via the physical short-press or the portal Mode form.
//
// Endpoints implemented:
//   GET /cm?cmnd=Power       -> status query, no state change
//   GET /cm?cmnd=Power%20ON  -> manual OPEN
//   GET /cm?cmnd=Power%20OFF -> manual CLOSED
//   GET /cm?cmnd=Power%20TOGGLE
//
// Multi-relay variants (`Power1`, `Power2` …) are accepted with any digit
// suffix so Moonraker configs specifying an output_id still work; we're a
// single relay so they all target the same vent.
static esp_err_t handle_tasmota_cm(httpd_req_t *req)
{
    char cmnd[32] = "";
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0) {
        char query[128];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            // form_get handles URL-decoding of %20 / '+' so "Power%20ON"
            // arrives here as "Power ON".
            form_get(query, "cmnd", cmnd, sizeof(cmnd));
        }
    }

    // Skip the "Power" verb and optional digit suffix; leave the argument.
    const char *arg = cmnd;
    if (strncasecmp(arg, "Power", 5) == 0) {
        arg += 5;
        while (isdigit((unsigned char)*arg)) arg++;
        while (*arg == ' ') arg++;
    }

    dv_motor_target_t current = dv_policy_get_target();
    bool want_on;
    if (strcasecmp(arg, "ON") == 0) {
        want_on = true;
    } else if (strcasecmp(arg, "OFF") == 0) {
        want_on = false;
    } else if (strcasecmp(arg, "TOGGLE") == 0) {
        want_on = (current != DV_MOTOR_TARGET_OPEN);
    } else {
        // No/unknown arg -> pure status query; report current position
        // without touching state.
        want_on = (current == DV_MOTOR_TARGET_OPEN);
        goto respond;
    }

    dv_motor_target_t t = want_on ? DV_MOTOR_TARGET_OPEN : DV_MOTOR_TARGET_CLOSED;
    dv_policy_set_manual_target(t);
    dv_policy_set_mode(DV_POLICY_MODE_MANUAL);
    dc_evlog_add("tasmota: POWER %s", want_on ? "ON" : "OFF");

respond:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
        want_on ? "{\"POWER\":\"ON\"}" : "{\"POWER\":\"OFF\"}");
}

static esp_err_t handle_factory_reset_post(httpd_req_t *req)
{
    ESP_LOGW(TAG, "factory reset requested from portal");
    dc_wifi_clear_creds();
    dc_moonraker_clear_config();
    dc_bambu_clear_config();
    dc_source_set(DC_SRC_KLIPPER);
    dv_policy_clear();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><body><h1>Factory reset. Rebooting…</h1></body>");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}

static esp_err_t handle_ap_post(httpd_req_t *req)
{
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    dc_wifi_ap_config_t cfg = {0};
    char ap_ssid[33] = {0}, ap_pass[65] = {0}, ap_ip[32] = {0}, ap_en[4] = {0};
    form_get(body, "ap_ssid",     ap_ssid, sizeof(ap_ssid));
    form_get(body, "ap_password", ap_pass, sizeof(ap_pass));
    form_get(body, "ap_ip",       ap_ip,   sizeof(ap_ip));
    // HTML checkboxes only submit when checked. Presence of the field means
    // enabled; absence means disabled.
    cfg.enabled = (form_get(body, "ap_enabled", ap_en, sizeof(ap_en)) == 0);

    // Empty inputs mean "revert to default" for that field.
    strncpy(cfg.ssid,     ap_ssid, sizeof(cfg.ssid) - 1);
    strncpy(cfg.password, ap_pass, sizeof(cfg.password) - 1);
    cfg.ip = ap_ip[0] ? parse_ipv4(ap_ip) : 0;

    // Weak sanity check: WPA2 needs ≥ 8 chars. Allow open (empty) too.
    if (cfg.password[0] != '\0' && strlen(cfg.password) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "password must be 8+ chars or blank");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><body><h1>AP config saved. Rebooting…</h1></body>");
    dc_wifi_set_ap_config_and_reboot(&cfg);   // does not return
    return ESP_OK;
}

// AP-mode catch-all: 302 to /, so captive-portal detectors trigger.
static esp_err_t handle_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------- start / stop ----------

static uint32_t get_ap_gateway_ip(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == NULL) return 0;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap, &info) != ESP_OK) return 0;
    return info.gw.addr;
}

esp_err_t dv_portal_start(void)
{
    if (s_httpd != NULL) return ESP_ERR_INVALID_STATE;
    s_ap_mode = (dc_wifi_state() == DC_WIFI_STATE_AP_PORTAL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // Default max_uri_handlers is 8 — we register 13 endpoints in STA and 14
    // in AP mode (including /* catchall for captive-portal detection). If
    // this is too low the last-registered handlers silently fail with
    // "no slots left" and captive portal stops firing.
    cfg.max_uri_handlers = 16;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t root  = { .uri = "/",           .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t wifi  = { .uri = "/wifi",       .method = HTTP_POST, .handler = handle_wifi_post };
    httpd_uri_t scan  = { .uri = "/scan",       .method = HTTP_POST, .handler = handle_scan_post };
    httpd_uri_t src   = { .uri = "/source",     .method = HTTP_POST, .handler = handle_source_post };
    httpd_uri_t mk    = { .uri = "/moonraker",  .method = HTTP_POST, .handler = handle_moonraker_post };
    httpd_uri_t bambu = { .uri = "/bambu",      .method = HTTP_POST, .handler = handle_bambu_post };
    httpd_uri_t mode  = { .uri = "/mode",       .method = HTTP_POST, .handler = handle_mode_post };
    httpd_uri_t poly  = { .uri = "/policy",     .method = HTTP_POST, .handler = handle_policy_post };
    httpd_uri_t apcfg = { .uri = "/ap_config",  .method = HTTP_POST, .handler = handle_ap_post };
    httpd_uri_t vent  = { .uri = "/vent",       .method = HTTP_POST, .handler = handle_vent_post };
    httpd_uri_t ota   = { .uri = "/ota",        .method = HTTP_POST, .handler = handle_ota_post };
    httpd_uri_t reset = { .uri = "/factory_reset", .method = HTTP_POST, .handler = handle_factory_reset_post };
    httpd_uri_t cm    = { .uri = "/cm",          .method = HTTP_GET,  .handler = handle_tasmota_cm };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &wifi);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &src);
    httpd_register_uri_handler(s_httpd, &mk);
    httpd_register_uri_handler(s_httpd, &bambu);
    httpd_register_uri_handler(s_httpd, &mode);
    httpd_register_uri_handler(s_httpd, &poly);
    httpd_register_uri_handler(s_httpd, &apcfg);
    httpd_register_uri_handler(s_httpd, &vent);
    httpd_register_uri_handler(s_httpd, &ota);
    httpd_register_uri_handler(s_httpd, &reset);
    httpd_register_uri_handler(s_httpd, &cm);

    // Kick off an initial WiFi scan so the SSID dropdown has entries by the
    // time the user loads the page.
    dc_wifi_scan_start();

    if (s_ap_mode) {
        uint32_t ip = get_ap_gateway_ip();
        if (ip != 0) dv_dns_start(ip);
        httpd_uri_t catchall = { .uri = "/*", .method = HTTP_GET, .handler = handle_captive_redirect };
        httpd_register_uri_handler(s_httpd, &catchall);
        ESP_LOGI(TAG, "portal up (AP mode, DNS on)");
    } else {
        ESP_LOGI(TAG, "portal up (STA mode)");
    }
    return ESP_OK;
}

esp_err_t dv_portal_stop(void)
{
    dv_dns_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    return ESP_OK;
}
