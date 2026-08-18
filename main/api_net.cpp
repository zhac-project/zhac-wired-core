// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_net.cpp -- replaces mono-core's api_wifi.cpp.
//
// Two surfaces on purpose:
//
//   /api/net/status   is the truth: link, speed, duplex, IP, gateway, MAC,
//                     mDNS hostname.
//   /api/wifi/status  is a compatibility shim so the shared www-spa keeps
//                     working unmodified. It reports mode="eth" with an empty
//                     ssid and rssi=0 -- the SPA renders those blank rather
//                     than being told a lie about a radio that is not there.
//                     Rewriting the SPA is out of scope, and SPA-side business
//                     logic is against project rule anyway.
//
// The write endpoints answer 501, not 404: 404 reads as "old firmware, wrong
// route", 501 reads as "this build cannot do that", which is accurate and far
// easier to diagnose from a browser console.
#include "api_net.h"

#include "ArduinoJson.h"
#include "esp_log.h"
#include "eth.h"
#include "net_discovery.h"

static const char* TAG = "api_net";

static esp_err_t send_json(httpd_req_t* req, const JsonDocument& doc) {
    // Guarded because serializeJson TRUNCATES on overflow rather than failing:
    // an oversized doc would ship a 200 carrying JSON cut off mid-key, which
    // every client reports as a parse error with nothing pointing back here.
    // Cheaper to 500 loudly. (/api/status hit exactly this when IPv6 fields
    // were added.)
    char buf[768];
    const size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        ESP_LOGE(TAG, "response overflow: %u B needed, %u B buffer",
                 (unsigned)measureJson(doc), (unsigned)sizeof(buf));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handle_net_status(httpd_req_t* req) {
    NetStatus st{};
    eth_get_status(&st);

    JsonDocument doc;
    doc["transport"]  = "ethernet";
    doc["link_up"]    = st.link_up;
    doc["has_ip"]     = st.has_ip;
    doc["ip"]         = st.ip;
    doc["netmask"]    = st.netmask;
    doc["gateway"]    = st.gw;
    doc["mac"]        = st.mac;
    doc["speed_mbps"] = st.speed_mbps;
    doc["duplex"]     = st.duplex_full ? "full" : "half";
    doc["hostname"]   = net_discovery_hostname();
    return send_json(req, doc);
}

// Compatibility shim -- same key set net-core's /api/wifi/status emits.
static esp_err_t handle_wifi_status_compat(httpd_req_t* req) {
    NetStatus st{};
    eth_get_status(&st);

    JsonDocument doc;
    doc["mode"] = "eth";
    doc["ssid"] = "";
    doc["ip"]   = st.ip;
    doc["rssi"] = 0;
    // Mono superset keys, so a SPA build reading them gets defined values
    // rather than undefined. "configured"/"connected" map onto link/IP, which
    // is the closest honest analogue.
    doc["sta_configured"] = st.link_up;
    doc["sta_connected"]  = st.has_ip;
    doc["sta_ssid"]       = "";
    doc["sta_ip"]         = st.ip;
    doc["sta_rssi"]       = 0;
    doc["ap_ssid"]        = "";
    return send_json(req, doc);
}

static esp_err_t handle_wifi_scan_compat(httpd_req_t* req) {
    JsonDocument doc;
    doc["networks"].to<JsonArray>();   // always empty
    return send_json(req, doc);
}

static esp_err_t handle_wifi_write_unsupported(httpd_req_t* req) {
    ESP_LOGW(TAG, "%s rejected -- this build has no WiFi", req->uri);
    httpd_resp_set_status(req, "501 Not Implemented");
    httpd_resp_set_type(req, "application/json");
    const char* body =
        "{\"error\":\"wifi_unsupported\","
        "\"detail\":\"zhac-wired-core is Ethernet-only; configure the network "
        "on your DHCP server or router\"}";
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// A plain {uri, method, handler} row rather than an httpd_uri_t literal:
// httpd_uri_t carries websocket fields this project never sets, and under
// -Wextra -Werror an aggregate initialiser that omits them is a hard error
// (-Werror=missing-field-initializers). Value-initialising and assigning the
// three fields we care about sidesteps that and survives IDF adding more.
namespace {
struct Route {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
};
}  // namespace

bool api_net_register(httpd_handle_t hd) {
    if (!hd) return false;
    static const Route kRoutes[] = {
        {"/api/net/status",      HTTP_GET,  handle_net_status},
        {"/api/wifi/status",     HTTP_GET,  handle_wifi_status_compat},
        {"/api/wifi",            HTTP_GET,  handle_wifi_status_compat},
        {"/api/wifi/scan",       HTTP_GET,  handle_wifi_scan_compat},
        {"/api/wifi/connect",    HTTP_POST, handle_wifi_write_unsupported},
        {"/api/wifi/disconnect", HTTP_POST, handle_wifi_write_unsupported},
        {"/api/wifi",            HTTP_POST, handle_wifi_write_unsupported},
    };
    for (const auto& r : kRoutes) {
        httpd_uri_t u{};
        u.uri     = r.uri;
        u.method  = r.method;
        u.handler = r.handler;
        if (httpd_register_uri_handler(hd, &u) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s", r.uri);
            return false;
        }
    }
    return true;
}
