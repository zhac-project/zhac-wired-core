// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth.h"
#include "api_system.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "mqtt_glue.h"
#include "ntp_cfg.h"
#include "sys_state.h"
#include "zigbee_diagnostics.h"
#include "log_ring.h"
#include "esp_heap_caps.h"
#include "ws_bridge.h"

static const char* TAG = "api_system";

extern "C" void metrics_mqtt_publisher_start();   // metrics_mqtt.cpp

// ── shared logic ──────────────────────────────────────────────────────────

bool system_apply_settings(const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) return false;

    // MQTT + Home Assistant discovery: persisted in NVS mqtt_cfg and applied
    // by mqtt_glue. (The mqtt_gw setters alone do NOT persist -- settings used
    // to vanish on reboot.)
    mqtt_glue_apply_settings(doc);

    // Time server: ntp_cfg persists it and restarts SNTP, no reboot.
    if (doc["ntp_server"].is<const char*>() &&
        !ntp_cfg_set_server(doc["ntp_server"].as<const char*>())) return false;

    // System flags (sys_state persists + applies).
    if (doc["timezone"].is<const char*>() && !sys_set_timezone(doc["timezone"].as<const char*>()))
        return false;   // not a POSIX TZ string
    if (doc["metrics_enabled"].is<bool>())
        sys_set_metrics_enabled(doc["metrics_enabled"].as<bool>());
    if (doc["metrics_mqtt_interval_s"].is<long>())
        sys_set_metrics_mqtt_interval_s(doc["metrics_mqtt_interval_s"].as<long>());
    if (doc["metrics_mqtt_enabled"].is<bool>())
        sys_set_metrics_mqtt_enabled(doc["metrics_mqtt_enabled"].as<bool>());
    // Starts the publisher task the first time the stream is on; once running
    // it also takes the Home Assistant sensors down when the stream goes off.
    metrics_mqtt_publisher_start();
    if (doc["ap_disabled"].is<bool>())
        sys_set_ap_disabled(doc["ap_disabled"].as<bool>());
    if (doc["auth_enabled"].is<bool>())
        sys_set_auth_enabled(doc["auth_enabled"].as<bool>());

    // "Allow script changes from the cloud" (spec 2026-10-05 §3.1). Only this hub's own page gets here with
    // the key: the cloud relay answers settings.set carrying it with local_only. Only JSON true turns it
    // on; any other value (false, 0, "true", {}) turns it off, so a malformed request can never leave it
    // on. null counts as absent, as it does for the relay's local_only check. A change pushes hub.caps,
    // so the cloud knows at once.
    if (!doc["remote_scripts"].isNull()) {
        const bool on = doc["remote_scripts"].is<bool>() && doc["remote_scripts"].as<bool>();
        if (on != sys_remote_scripts()) {
            sys_set_remote_scripts(on);
            ESP_LOGW(TAG, "script changes from the cloud: %s", on ? "allowed" : "off");
            ws_push_hub_caps();
        }
    }

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
    auth_register_uri(hd, &u);

    u.uri = "/api/token/rotate"; u.method = HTTP_POST;
    u.handler = handle_token_rotate;
    auth_register_uri(hd, &u);

    u.uri = "/api/system/token/rotate"; u.method = HTTP_POST;   // net-core URI alias
    u.handler = handle_token_rotate;
    auth_register_uri(hd, &u);

    u.uri = "/api/diagnostics/unhandled"; u.method = HTTP_GET;
    u.handler = handle_diagnostics_unhandled;
    auth_register_uri(hd, &u);

    u.uri = "/api/logs"; u.method = HTTP_GET;
    u.handler = handle_logs_get;
    auth_register_uri(hd, &u);

    ESP_LOGI(TAG, "settings / token / diagnostics routes registered");
    return true;
}
