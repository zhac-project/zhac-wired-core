// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_status.cpp — `GET /api/status` runtime snapshot.
//
// Single-chip: every field net-core split across S3 + the P4 `s_p4_*`
// heartbeat cache is read directly here (same chip). No HAP roundtrip.
#include "api_status.h"

#include <cinttypes>
#include <cstdio>

#include "ArduinoJson.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "eth.h"
#include "sys_diag.h"
#include "mqtt_gw.h"
#include "net_discovery.h"
#include "radio_state.h"
#include "sdkconfig.h"
#include "sys_state.h"
#include "ws_server.h"
#include "zigbee_pool.h"
#include "log_ring.h"

static const char* TAG = "api_status";

static esp_err_t handle_get_status(httpd_req_t* req) {
    JsonDocument doc;

    // Chip / build
    esp_chip_info_t info{};
    esp_chip_info(&info);
    doc["target"]   = CONFIG_IDF_TARGET;
    doc["cores"]    = info.cores;
    doc["revision"] = info.revision;
    const esp_app_desc_t* app = esp_app_get_description();
    if (app) doc["fw"] = app->version;

    doc["uptime_s"] = (uint32_t)(esp_timer_get_time() / 1000000);

    // Full diagnostics set, identical to the WS status.get payload (cpu_c0,
    // int_free, heap_min, stack_hwm, ...). The pre-existing heap_* / uptime_s
    // keys below are KEPT: they are this SKU's own REST shape and something
    // may already scrape them. Both live side by side rather than one being
    // renamed out from under a consumer.
    sys_diag_fill(doc.as<JsonObject>(), SYS_DIAG_CPU_ONDEMAND);

    // Heap (internal + PSRAM: free / min / largest block)
    doc["heap_internal_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    doc["heap_internal_min"]  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    doc["heap_internal_blk"]  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    doc["heap_total_free"]    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (esp_psram_is_initialized()) {
        doc["psram_size"] = (uint32_t)esp_psram_get_size();
        doc["psram_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        doc["psram_min"]  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        doc["psram_blk"]  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    } else {
        doc["psram_size"] = 0;
    }

    // Network. The `ip` key keeps its name -- the shared SPA reads it, and
    // renaming would silently blank the status card. `wifi_mode` keeps its
    // name too, reporting "eth", so a SPA that switches on it sees a defined
    // value instead of undefined. Everything wired-specific is additive.
    NetStatus net{};
    eth_get_status(&net);
    doc["wifi_mode"]     = "eth";          // compat: there is no radio here
    doc["ip"]            = net.ip;
    doc["net_transport"] = "ethernet";
    doc["net_link_up"]   = net.link_up;
    doc["net_speed"]     = net.speed_mbps;
    doc["net_duplex"]    = net.duplex_full ? "full" : "half";
    doc["hostname"]      = net_discovery_hostname();
    // No `rssi` key: this is copper. Emitting 0 would read as "terrible
    // signal" rather than "not applicable".

    // Zigbee + devices. Sourced from the device_backend registry rather than
    // zigbee_mgr_crashed(), so this stays correct across the Phase 1 radio
    // cutover -- see radio_state.h. In Phase 0 no backend is registered, so
    // both report false and device_count is 0, which is the truth.
    doc["zigbee_present"] = radio_present();
    doc["zigbee_ok"]      = radio_ok();
    doc["device_count"]   = pool_count_active();

    // MQTT
    doc["mqtt_connected"]  = mqtt_gw_is_connected();
    doc["mqtt_active"]     = mqtt_gw_is_active();
    doc["mqtt_root_topic"] = mqtt_gw_get_root_topic();

    // WS + system flags
    doc["ws_clients"]      = ws_server_client_count();
    doc["metrics_enabled"] = sys_metrics_enabled();
    doc["ap_disabled"]     = sys_ap_disabled();
    doc["auth_enabled"]    = sys_auth_enabled();
    doc["log_mqtt_enabled"] = log_sinks_get_mqtt_enabled();
    doc["log_ws_enabled"]   = log_sinks_get_ws_enabled();
#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    doc["remote_available"] = true;
#else
    doc["remote_available"] = false;
#endif

    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

bool api_status_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    u.uri     = "/api/status";
    u.method  = HTTP_GET;
    u.handler = handle_get_status;
    esp_err_t e = httpd_register_uri_handler(hd, &u);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "register failed: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "GET /api/status registered");
    return true;
}
