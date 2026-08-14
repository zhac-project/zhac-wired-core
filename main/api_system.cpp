// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "api_system.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "mqtt_gw.h"
#include "sys_state.h"
#include "zigbee_diagnostics.h"
#include "log_ring.h"
#include "esp_heap_caps.h"

static const char* TAG = "api_system";

// ── shared logic ──────────────────────────────────────────────────────────

bool system_apply_settings(const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) return false;

    // MQTT — the mqtt_gw setters self-persist (NVS mqtt_cfg) and restart the
    // client as needed, so we just forward.
    if (doc["broker_url"].is<const char*>())
        mqtt_gw_set_broker_url(doc["broker_url"].as<const char*>());
    if (doc["mqtt_root_topic"].is<const char*>())
        mqtt_gw_set_root_topic(doc["mqtt_root_topic"].as<const char*>());
    if (doc["mqtt_client_id"].is<const char*>())
        mqtt_gw_set_client_id(doc["mqtt_client_id"].as<const char*>());
    if (doc["mqtt_enabled"].is<bool>()) {
        if (doc["mqtt_enabled"].as<bool>()) mqtt_gw_on_sta_up();
        else                                 mqtt_gw_stop();
    }

    // System flags (sys_state persists + applies).
    if (doc["timezone"].is<const char*>())
        sys_set_timezone(doc["timezone"].as<const char*>());
    if (doc["metrics_enabled"].is<bool>())
        sys_set_metrics_enabled(doc["metrics_enabled"].as<bool>());
    if (doc["ap_disabled"].is<bool>())
        sys_set_ap_disabled(doc["ap_disabled"].as<bool>());
    if (doc["auth_enabled"].is<bool>())
        sys_set_auth_enabled(doc["auth_enabled"].as<bool>());

    // Live-log sinks (MQTT / WS), enable + min-level (level = first char).
    if (doc["log_mqtt_enabled"].is<bool>() || doc["log_mqtt_level"].is<const char*>()) {
        bool en = doc["log_mqtt_enabled"] | log_sinks_get_mqtt_enabled();
        const char* lvl = doc["log_mqtt_level"] | (const char*)nullptr;
        log_sinks_set_mqtt(en, (lvl && lvl[0]) ? lvl[0] : log_sinks_get_mqtt_level());
    }
    if (doc["log_ws_enabled"].is<bool>() || doc["log_ws_level"].is<const char*>()) {
        bool en = doc["log_ws_enabled"] | log_sinks_get_ws_enabled();
        const char* lvl = doc["log_ws_level"] | (const char*)nullptr;
        log_sinks_set_ws(en, (lvl && lvl[0]) ? lvl[0] : log_sinks_get_ws_level());
    }
    return true;
}

bool system_rotate_token(char* out, size_t cap) {
    return sys_rotate_api_token(out, cap);
}

size_t system_diagnostics_json(char* out, size_t cap) {
    ZbUnhandledFrame fr[24];
    uint16_t n = zb_diag_snapshot(fr, 24);

    JsonDocument doc;
    JsonArray arr = doc["entries"].to<JsonArray>();
    const uint32_t now = (uint32_t)time(nullptr);
    for (uint16_t i = 0; i < n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["cluster"] = fr[i].cluster_id;
        o["id"]      = fr[i].attr_or_cmd_id;
        o["cs"]      = fr[i].cluster_specific;
        o["count"]   = fr[i].count;
        o["age_s"]   = now > fr[i].last_seen_s ? now - fr[i].last_seen_s : 0;
        char ib[19];
        snprintf(ib, sizeof(ib), "0x%016" PRIx64, fr[i].last_ieee);
        o["ieee"] = ib;
    }
    return serializeJson(doc, out, cap);
}

// ── REST wrappers ───────────────────────────────────────────────────────────

static esp_err_t handle_settings_set(httpd_req_t* req) {
    char buf[512];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';
    bool ok = system_apply_settings(buf, (size_t)n);
    httpd_resp_set_type(req, "application/json");
    if (ok) return httpd_resp_sendstr(req, "{\"ok\":true}");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
}

static esp_err_t handle_token_rotate(httpd_req_t* req) {
    char tok[33];
    httpd_resp_set_type(req, "application/json");
    if (!system_rotate_token(tok, sizeof(tok))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
        return ESP_FAIL;
    }
    char r[64];
    int rl = snprintf(r, sizeof(r), "{\"ok\":true,\"token\":\"%s\"}", tok);
    return httpd_resp_send(req, r, rl);
}

static esp_err_t handle_diagnostics_unhandled(httpd_req_t* req) {
    char buf[1536];
    size_t n = system_diagnostics_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (n == 0) return httpd_resp_sendstr(req, "{\"entries\":[]}");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handle_logs_get(httpd_req_t* req) {
    constexpr size_t CAP = 32768;            // PSRAM — log JSON can be large
    char* buf = static_cast<char*>(heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t n = log_ring_to_json(buf, CAP);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = n ? httpd_resp_send(req, buf, n)
                    : httpd_resp_sendstr(req, "{\"logs\":[]}");
    heap_caps_free(buf);
    return e;
}

bool api_system_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};

    u.uri = "/api/settings"; u.method = HTTP_POST;
    u.handler = handle_settings_set;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/token/rotate"; u.method = HTTP_POST;
    u.handler = handle_token_rotate;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/system/token/rotate"; u.method = HTTP_POST;   // net-core URI alias
    u.handler = handle_token_rotate;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/diagnostics/unhandled"; u.method = HTTP_GET;
    u.handler = handle_diagnostics_unhandled;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/logs"; u.method = HTTP_GET;
    u.handler = handle_logs_get;
    httpd_register_uri_handler(hd, &u);

    ESP_LOGI(TAG, "settings / token / diagnostics routes registered");
    return true;
}
