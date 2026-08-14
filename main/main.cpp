// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zhac-wired-core / main.cpp
//
// Single-chip ESP32-P4 boot. Same shape as zhac-mono-core's app_main with two
// differences, both deliberate:
//
//   * esp_eth + mDNS instead of wifi_start(). No credentials, no AP fallback,
//     no provisioning -- discovery is DHCP plus <hostname>.local.
//   * NO radio. znp_driver_init() / zigbee_backend_register() /
//     zigbee_mgr_init() are absent; the device_backend registry stays empty
//     and /api/devices returns an empty list, which is the correct answer
//     rather than a stub's fiction. Phase 1 registers esp_zigbee_backend
//     (esp-zigbee-lib in UART_RCP mode driving a C6 running stock ot_rcp) and
//     nothing else here has to change.
//
// Everything between those two -- shadow, store, adapter, diagnostics, rules,
// Lua, WS, MQTT, SPA -- is up and exercised, so Phase 1 is a backend
// registration rather than a re-architecture.
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

// Bridge shim -- both header families live in components/ here and provide the
// in-process replacement for the dual-chip SPI link.
#include "hap_master.h"
#include "hap_protocol.h"
#include "hap_slave.h"

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"

#include "api_devices.h"
#include "api_groups.h"
#include "api_net.h"
#include "api_remote.h"
#include "api_rules.h"
#include "api_scripts.h"
#include "api_status.h"
#include "api_system.h"
#include "device_options.h"
#include "device_shadow.h"
#include "eth.h"
#include "event_bus.h"
#include "log_ring.h"
#include "lua_engine.h"
#include "mqtt_gw.h"
#include "net_discovery.h"
#include "remote_client.h"
#include "simple_rules.h"
#include "spa_serve.h"
#include "sys_state.h"
#include "task_stacks.h"   // zhac::stack::kEventBus (zap_common)
#include "ws_bridge.h"
#include "ws_server.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "zigbee_diagnostics.h"

extern "C" void lua_engine_rules_hook_install(void);
extern "C" void metrics_mqtt_publisher_start();   // metrics_mqtt.cpp

#include <cinttypes>

static const char* TAG = "zhac-wired";

static void log_chip_info() {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    ESP_LOGI(TAG, "chip: %s, rev v%u.%u, %d cores, flash %" PRIu32 " MB",
             CONFIG_IDF_TARGET,
             info.revision / 100,
             info.revision % 100,
             info.cores,
             flash_size / (1024 * 1024));
}

static void log_psram_info() {
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not initialized -- this build REQUIRES PSRAM");
        return;
    }
    ESP_LOGI(TAG, "PSRAM: initialized, %u MB",
             (unsigned)(esp_psram_get_size() / (1024 * 1024)));
}

static void log_heap_info() {
    ESP_LOGI(TAG, "heap internal: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "heap spiram:   free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// ── Event-bus pump ───────────────────────────────────────────────────
// The event bus delivers into per-subscriber queues; NOTHING runs a
// subscriber until a pump task drains its type. On dual-chip this pump lives
// on the P4; single-chip must run its own. Without it every ws_bridge
// subscriber (attr.changed WS push, device join/leave, optimistic-shadow
// forward) AND any rule or Lua reaction to a device attr change is silently
// inert while the firmware looks perfectly healthy.
//
// The drain range is derived from the enum (1 .. _COUNT-1) and must stay that
// way: a hardcoded 1..10 list on the P4 silently stopped draining a
// newly-added type (SHADOW_OPTIMISTIC = 11), whose subscriber queue then
// filled forever. Draining a type with no subscribers is a cheap no-op.
static void task_event_bus(void*) {
    ESP_LOGI(TAG, "TaskEventBus started");
    while (true) {
        uint8_t processed = 0;
        for (uint8_t t = 1; t < static_cast<uint8_t>(EventType::_COUNT); t++) {
            processed += event_bus_drain(static_cast<EventType>(t), 0);
        }
        if (processed == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));   // idle yield; matches P4 cadence
        }
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "boot -- zhac-wired-core (P4, wired Ethernet, no radio)");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase -- reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    // System flags + API-auth token (NVS-backed). Must follow nvs_flash_init.
    sys_state_init();
    log_ring_init();   // PSRAM log ring + esp_log vprintf hook (capture early)

    log_chip_info();
    log_psram_info();
    log_heap_info();

    hap_master_init();
    hap_slave_init();

    // ── Device pipeline (no radio backend) ───────────────────────────
    // Order is load-bearing: event_bus first because other subsystems publish
    // into it, then the NVS-backed stores, then the device-definition adapter.
    // Mono continues here with znp_driver_init() / zigbee_backend_register() /
    // zigbee_mgr_init(); this build deliberately stops short. Nothing
    // registers itself in the device_backend registry, so radio_present() is
    // false and the device list is empty until Phase 1.
    event_bus_init();
    zap_store_init();
    zap_store_flush_init();
    device_shadow_init();
    zhac_adapter_init();
    zb_diag_init();   // unhandled-frame ring for GET /api/diagnostics/unhandled

    // Re-apply persisted per-device options now that device_shadow and the
    // device pool are up. A no-op while the pool is empty.
    device_options_restore_all();

    // ── Rules + Lua ──────────────────────────────────────────────────
    // simple_rules first (NVS-backed store + rule cache), then lua_engine
    // (spins up TaskLua but defers loading script sources), then the glue that
    // lets a rule's `script.run` action push onto the Lua scheduler. Script
    // loading is deferred to the end of app_main so a script touching the
    // network or HTTP stack at top level cannot race it.
    simple_rules_init();
    const bool lua_ok = lua_engine_init();
    if (!lua_ok) {
        ESP_LOGW(TAG, "lua_engine_init returned false -- scripts disabled");
    }
    lua_engine_rules_hook_install();

    // ── Network ──────────────────────────────────────────────────────
    eth_start();
    net_discovery_start(CONFIG_ZHAC_MDNS_HOSTNAME);

    // Mount the SPA partition before registering httpd routes so the catchall
    // can find index.html. An empty partition is fine: asset requests 404 and
    // every /api/* route still works.
    spa_mount();

    // ── HTTP + WS ────────────────────────────────────────────────────
    // ws_server owns the httpd instance; everything else registers against its
    // handle. Unlike mono there is no placeholder "/" handler -- it would
    // shadow the SPA's index, and spa_register()'s catchall already serves "/".
    ws_server_init();
    httpd_handle_t hd = ws_server_get_handle();
    if (hd) {
        api_status_register(hd);
        api_devices_register(hd);
        api_net_register(hd);      // replaces mono's api_wifi_register
        api_rules_register(hd);
        api_scripts_register(hd);
        api_system_register(hd);
        api_groups_register(hd);
        api_remote_register(hd);
        // The SPA catchall MUST register last: its `/*` pattern would
        // otherwise shadow the specific /api/* routes, because
        // esp_http_server only falls back to a wildcard when no earlier
        // handler claims the URI.
        spa_register(hd);
        ESP_LOGI(TAG, "HTTP routes registered");
    } else {
        ESP_LOGE(TAG, "ws_server has no httpd handle -- REST and SPA are down");
    }

    // WS RX dispatcher + outbound event-bus push subscriptions.
    ws_bridge_install();

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    // Feed link up/down into the remote client's state machine. The bit
    // positions must match EVB_WIFI_UP = 1<<2 / EVB_WIFI_DOWN = 1<<3 in
    // remote_client.cpp, which lives in a read-only sibling repo -- so the
    // names stay WiFi-flavoured even though the events are now Ethernet's.
    // Only the source changes.
    extern EventGroupHandle_t s_remote_evt;   // owned by remote_client.cpp
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
        [](void*, esp_event_base_t, int32_t, void*) {
            if (s_remote_evt) xEventGroupSetBits(s_remote_evt, 1 << 2);
        }, nullptr);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
        [](void*, esp_event_base_t, int32_t, void*) {
            if (s_remote_evt) xEventGroupSetBits(s_remote_evt, 1 << 3);
        }, nullptr);
#endif
    remote_client_init();   // no-op stub when remote disabled; reads NVS

    // mqtt_gw is config-gated: with no broker URL provisioned in NVS,
    // mqtt_gw_start() logs and idles until mqtt_gw_configure() arrives from
    // the REST handler. Boots cleanly either way.
    mqtt_gw_init();
    mqtt_gw_start();
    metrics_mqtt_publisher_start();   // no-op if the exporter is off

    if (lua_ok) {
        // Safe now: every subsystem a script might call into is initialised.
        lua_engine_load_all();
        ESP_LOGI(TAG, "lua_engine: scripts loaded");
    }

    // Start the pump LAST: every service its subscribers reach (ws_server,
    // remote_client, mqtt) is initialised, so a drained event cannot call into
    // an uninitialised subsystem. Stack and priority mirror the P4's
    // TaskEventBus -- the pump runs simple_rules' dispatch_event ->
    // execute_rule in THIS task's context, so it needs the same depth; a
    // smaller stack risks a runtime overflow. Priority 2 sits below lwIP so
    // networking stays responsive.
    if (xTaskCreate(task_event_bus, "TaskEventBus", zhac::stack::kEventBus,
                    nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "TaskEventBus create FAILED -- event subscribers inert");
    } else {
        ESP_LOGI(TAG, "TaskEventBus up -- event subscribers now serviced");
    }

    int tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        NetStatus net{};
        eth_get_status(&net);
        ESP_LOGI(TAG,
                 "alive (tick=%d, uptime=%" PRId64 " s) net: link=%d ip=%s "
                 "speed=%" PRIu32 " duplex=%s",
                 ++tick, esp_timer_get_time() / 1000000,
                 (int)net.link_up,
                 net.ip[0] ? net.ip : "-",
                 net.speed_mbps,
                 net.duplex_full ? "full" : "half");
        if ((tick % 6) == 0) log_heap_info();
    }
}
