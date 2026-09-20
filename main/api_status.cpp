// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_status.cpp — `GET /api/status` runtime snapshot.
//
// Single-chip: every field net-core split across S3 + the P4 `s_p4_*`
// heartbeat cache is read directly here (same chip). No HAP roundtrip.
#include "auth.h"
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
#include "mqtt_glue.h"
#include "mqtt_gw.h"
#include "net_discovery.h"
#include "ntp_cfg.h"
#include "ota_update.h"
#include "radio_state.h"
#include "sdkconfig.h"
#include "esp_zigbee_backend.h"   // esp_zigbee_backend_last_error
#include "sys_state.h"
#include "ws_server.h"
#include "zigbee_pool.h"
#include "log_ring.h"

static const char* TAG = "api_status";

void api_status_fill(JsonObject doc) {
    doc["sku"]      = "wired";
    doc["ota"]      = true;       // ota.update: the web UI shows its update field
    doc["ota_state"] = ota_update_state();   // "pending" while a new image is on trial
    if (const char* why = ota_update_rollback_reason()) doc["ota_rollback_reason"] = why;

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
    sys_diag_fill(doc, SYS_DIAG_CPU_ONDEMAND);

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
    doc["ip6"]           = net.ip6_global[0] ? net.ip6_global : net.ip6_link_local;
    doc["ip6_ll"]        = net.ip6_link_local;
    doc["ip6_global"]    = net.ip6_global;
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
#if CONFIG_ZHAC_ESP_ZIGBEE
    if (const char* e = esp_zigbee_backend_last_error()) doc["radio_error"] = e;
#endif
    doc["device_count"]   = pool_count_active();

    // MQTT
    doc["mqtt_connected"]  = mqtt_gw_is_connected();
    doc["mqtt_active"]     = mqtt_gw_is_active();
    doc["mqtt_root_topic"] = mqtt_gw_get_root_topic();
    mqtt_glue_fill_status(doc);   // mqtt_enabled, mqtt_broker, ha_discovery, ...

    // WS + system flags
    doc["ws_clients"]      = ws_server_client_count();
    doc["metrics_enabled"] = sys_metrics_enabled();
    doc["ap_disabled"]     = sys_ap_disabled();
    doc["auth_enabled"]    = sys_auth_enabled();
    // Drives the web UI's one-time "set admin password" card. Discloses only
    // that no password exists yet -- the same fact /api/auth/setup makes true.
    doc["auth_setup_required"] = sys_auth_enabled() && !auth_password_is_set();
    doc["auth_setup_secs_left"] = auth_setup_secs_left();   // 0 = closed; power-cycle the hub to reopen
    if (auth_storage_error()) doc["auth_storage_error"] = true;   // sign-in forced on, serial token only
    // Schedules (cron rules, Lua on_cron) wait until SNTP has set the clock;
    // the web UI's Rules page says so while this is false.
    doc["clock_set"] = esp_zigbee_backend_wall_clock_s() != 0;
    doc["ntp_server"] = ntp_cfg_server();
    {   // the router's offer in use (DHCP option 42), when there is one
        char dhcp[48];
        if (ntp_cfg_dhcp_server(dhcp, sizeof(dhcp))) doc["ntp_dhcp_server"] = dhcp;
    }
    doc["log_mqtt_enabled"] = log_sinks_get_mqtt_enabled();
    doc["log_ws_enabled"]   = log_sinks_get_ws_enabled();
#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    doc["remote_available"] = true;
#else
    doc["remote_available"] = false;
#endif
}

static esp_err_t handle_get_status(httpd_req_t* req) {
    JsonDocument doc;
    api_status_fill(doc.to<JsonObject>());
    // 2048, not 1024: this doc carries the full sys_diag set (cpu, heap, psram,
    // stack, net) plus this SKU's own legacy keys, and adding four IPv6 fields
    // pushed it past 1024. serializeJson TRUNCATES rather than failing, so the
    // overflow shipped a 200 with a JSON document cut off mid-key -- every
    // client just sees a parse error. Same trap as ws_push (fixed there too).
    // Now 2560 on the heap: the same doc also answers WS status.get, which
    // added the sku / settings keys the web UI reads.
    constexpr size_t CAP = 2560;
    char* buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    const size_t n = serializeJson(doc, buf, CAP);
    if (n == 0 || n >= CAP) {
        ESP_LOGE(TAG, "/api/status overflow: %u B needed, %u B buffer",
                 (unsigned)measureJson(doc), (unsigned)CAP);
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_send(req, buf, n);
    heap_caps_free(buf);
    return err;
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
