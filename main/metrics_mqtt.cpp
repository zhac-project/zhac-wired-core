// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// metrics_mqtt.cpp -- Settings > Misc > "Stream metrics to MQTT".
//
// While the stream is on, Metrics is on and the broker is connected, every
// 1-60 s (sys_metrics_mqtt_interval_s) it publishes the Info page's Resources
// figures plus device count, link and cloud state (api_status_fill_metrics,
// the /api/status builder) to <root>/bridge/metrics, QoS 0, not retained --
// next to the log stream's <root>/log/<level> (log_ring.cpp), with the same
// connection check.
//
// With Home Assistant discovery on it also keeps diagnostic sensors on the
// hub's own device (ha::build_bridge_metrics), and takes them down again with
// empty retained configs once the stream, Metrics or discovery goes off, or
// the root topic or discovery prefix moves.
//
// Its own task, not ha_bridge's (a few hundred bytes of stack to spare) nor
// esp_timer's (shared, internal). The stack is in PSRAM (zhac_task_create):
// internal RAM pays only the TCB, and only once the stream has been switched
// on. The task never writes flash -- the settings handler persists -- which a
// PSRAM stack requires.
#include <atomic>
#include <cstdio>
#include <cstring>

#include "ArduinoJson.h"
#include "api_status.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_bridge.h"
#include "mqtt_gw.h"
#include "sys_state.h"
#include "zhac_task.h"

namespace {

constexpr const char* TAG = "metrics_mqtt";
// PSRAM, so generous. Deepest paths by -fstack-usage (S31 build): the HA
// configs, task 304 + ha::build_bridge_metrics 1360 + emit_config 192 +
// mqtt_gw_publish 224 + heap/queue ~250 = ~2.3 KB; an ESP_LOG through
// log_ring's hook (480) and the ROM vprintf about the same. ~2.5x headroom.
constexpr uint32_t kStack      = 6144;
constexpr size_t   kPayloadCap = 512;   // ~370 B at worst

std::atomic<bool> s_started{false};

// The stream may run every second: keep its JSON pool out of internal RAM.
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
PsramAllocator s_psram;

void publish_metrics() {
    char topic[64];
    if (mqtt_gw_format_topic(topic, sizeof(topic), "bridge/metrics") <= 0) return;
    JsonDocument doc(&s_psram);
    api_status_fill_metrics(doc.to<JsonObject>());
    char* buf = static_cast<char*>(heap_caps_malloc(kPayloadCap, MALLOC_CAP_SPIRAM));
    if (!buf) return;
    const size_t n = doc.overflowed() ? 0 : serializeJson(doc, buf, kPayloadCap);
    if (n > 0 && n < kPayloadCap) mqtt_gw_publish(topic, buf, n, 0, false);
    heap_caps_free(buf);
}

// Where the Home Assistant configs sit on the broker (prefix "" = nowhere),
// so they can be taken down from there after a move.
char s_ha_prefix[32] = "";
char s_ha_root[32]   = "";
bool s_ha_fresh      = false;   // (re)published on this broker connection

struct Emit { bool retract; bool ok; };

void emit_config(const char*, const char* topic, const char* payload, void* user) {
    auto* e = static_cast<Emit*>(user);
    char abs[168];   // leading '/': an absolute topic, not under <root>
    const int n = snprintf(abs, sizeof(abs), "/%s", topic);
    const char* body = e->retract ? "" : payload;
    const bool sent = n > 0 && n < static_cast<int>(sizeof(abs)) &&
                      mqtt_gw_publish(abs, body, strlen(body), 1, true);
    e->ok = e->ok && sent;
}

bool send_configs(const char* prefix, const char* root, bool retract) {
    char bid[32];
    ha::bridge_id(bid, sizeof(bid), root);
    Emit e{retract, true};
    ha::build_bridge_metrics({prefix, root, bid}, emit_config, &e);
    return e.ok;   // false: mqtt_gw's queue was full -- all of it again next second
}

void sync_home_assistant(bool want) {
    // Copies: the settings handler rewrites both on another task.
    char prefix[sizeof(s_ha_prefix)], root[sizeof(s_ha_root)];
    snprintf(prefix, sizeof(prefix), "%s", ha_bridge_prefix());
    snprintf(root, sizeof(root), "%s", mqtt_gw_get_root_topic());
    const bool moved = s_ha_prefix[0] &&
                       (strcmp(prefix, s_ha_prefix) != 0 || strcmp(root, s_ha_root) != 0);
    if (s_ha_prefix[0] && (!want || moved)) {
        if (!send_configs(s_ha_prefix, s_ha_root, true)) return;
        ESP_LOGI(TAG, "Home Assistant sensors removed from %s/", s_ha_prefix);
        s_ha_prefix[0] = '\0';
    }
    // Again on every new connection: a restarted broker may have lost them.
    if (want && (!s_ha_prefix[0] || !s_ha_fresh)) {
        if (!send_configs(prefix, root, false)) return;
        if (!s_ha_prefix[0]) ESP_LOGI(TAG, "Home Assistant sensors published under %s/", prefix);
        memcpy(s_ha_prefix, prefix, sizeof(prefix));
        memcpy(s_ha_root, root, sizeof(root));
        s_ha_fresh = true;
    }
}

void task(void*) {
    TickType_t wake = xTaskGetTickCount();
    int due = 0;   // seconds to the next publish; <= 0: now
    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        if (!mqtt_gw_is_connected()) { s_ha_fresh = false; due = 0; continue; }
        const bool on = sys_metrics_mqtt_enabled() && sys_metrics_enabled();
        sync_home_assistant(on && ha_bridge_enabled());
        if (!on) { due = 0; continue; }
        const int every = sys_metrics_mqtt_interval_s();
        if (due > every) due = every;   // a shorter interval applies at once
        if (--due <= 0) {
            publish_metrics();
            due = every;
        }
    }
}

}  // namespace

// Starts the task the first time the stream is on (boot, or the setting);
// idempotent. It then stays, idle, so that turning the stream off can still
// take the Home Assistant sensors down.
extern "C" void metrics_mqtt_publisher_start() {
    if (!sys_metrics_mqtt_enabled() || s_started.exchange(true)) return;
    if (zhac_task_create(task, "metrics_mqtt", kStack, nullptr, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "task start failed -- no metrics stream");
        return;
    }
    ESP_LOGI(TAG, "stream on: <root>/bridge/metrics every %d s", sys_metrics_mqtt_interval_s());
}
