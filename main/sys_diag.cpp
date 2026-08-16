// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sys_diag -- runtime diagnostics for the Info page.
//
// KEY NAMES ARE A CONTRACT
// ------------------------
// www-spa's Info.jsx reads these exact keys. They are NOT free to rename:
//
//   cpu_c0 cpu_c1                       CPU busy percent per core
//   heap heap_min                       total heap (internal + PSRAM)
//   int_free int_min int_blk            internal DRAM only
//   psram_free psram_min psram_blk psram_total
//   stack_hwm                           smallest task stack headroom
//   uptime fw_version ip mac ws_clients mqtt_connected wifi synced
//
// The names match zhac-net-core's /api/status so the same SPA renders both
// SKUs. Any change here is a change to the SPA's data contract.
#include "sys_diag.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_gw.h"
#include "sys_metrics.h"
#include "sys_state.h"
#include "ws_bridge.h"
#include "ws_server.h"

#include <cstdio>
#include <cstdlib>

namespace {

// One rolling window per cadence -- see SysDiagCpuSlot. Sharing a window
// between the 10 s tick and an on-demand request would make both report
// nonsense, because each call consumes the delta the other was measuring.
sys_metrics_cpu_ctx_t s_cpu_ctx[2] = {};

// Smallest remaining stack across every live task, in bytes.
//
// zhac-net-core walks zap_common's `zhac::stack::kTable`, but that table is
// chip-conditional (#if ESP32P4 / #elif ESP32S3) and therefore EMPTY on
// esp32s31 -- copying that approach here would have reported a confident 0.
// Enumerating the live task list is target-agnostic and also covers tasks the
// table never listed (lwIP, httpd, the esp-zigbee stack's own threads), which
// is exactly where a stack overflow would actually come from.
uint32_t min_stack_headroom(char* worst_name, size_t cap) {
    if (worst_name && cap) worst_name[0] = '\0';
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    const UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) return 0;
    auto* arr = static_cast<TaskStatus_t*>(calloc(n, sizeof(TaskStatus_t)));
    if (!arr) return 0;
    const UBaseType_t got = uxTaskGetSystemState(arr, n, nullptr);
    uint32_t min_free = UINT32_MAX;
    for (UBaseType_t i = 0; i < got; i++) {
        const uint32_t free_b = static_cast<uint32_t>(arr[i].usStackHighWaterMark) *
                                static_cast<uint32_t>(sizeof(StackType_t));
        if (free_b < min_free) {
            min_free = free_b;
            // Naming the task is what makes the number actionable: "624 bytes
            // left" is alarming but useless until you know which stack to
            // grow in task_stacks.h.
            if (worst_name && cap && arr[i].pcTaskName) {
                snprintf(worst_name, cap, "%s", arr[i].pcTaskName);
            }
        }
    }
    free(arr);
    return (min_free == UINT32_MAX) ? 0 : min_free;
#else
    return 0;
#endif
}

}  // namespace

void sys_diag_fill(JsonObject d, SysDiagCpuSlot slot) {
    uint8_t c0 = 0, c1 = 0;
    sys_metrics_sample_cpu_pct(s_cpu_ctx[slot], c0, c1);
    d["cpu_c0"] = c0;
    d["cpu_c1"] = c1;

    // "heap" is the whole allocator (internal + PSRAM); "int_*" is internal
    // DRAM alone. On this SKU internal DRAM is the scarce one -- PSRAM has
    // megabytes free while internal sits in the tens of KB -- so the two must
    // stay distinguishable rather than collapsing into one number.
    d["heap"]     = static_cast<uint32_t>(esp_get_free_heap_size());
    d["heap_min"] = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    d["int_free"] = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    d["int_min"]  = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    d["int_blk"]  = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    if (esp_psram_is_initialized()) {
        d["psram_free"]  = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        d["psram_min"]   = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        d["psram_blk"]   = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        d["psram_total"] = static_cast<uint32_t>(esp_psram_get_size());
    } else {
        d["psram_free"] = 0; d["psram_min"] = 0;
        d["psram_blk"]  = 0; d["psram_total"] = 0;
    }

    char worst[configMAX_TASK_NAME_LEN] = {};
    d["stack_hwm"] = min_stack_headroom(worst, sizeof(worst));
    if (worst[0]) d["stack_hwm_task"] = worst;
    d["uptime"]    = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

    if (const esp_app_desc_t* app = esp_app_get_description()) {
        d["fw_version"] = app->version;
    }

    NetStatus net{};
    eth_get_status(&net);
    d["ip"]  = net.ip;
    d["mac"] = net.mac;

    // The SPA labels this row "WiFi" because it was written for the S3
    // gateway. This SKU has no WiFi at all -- its uplink is Ethernet. Reporting
    // false would paint a red "Disconnected" badge on a perfectly healthy wired
    // gateway, which is worse than a mislabelled row, so it carries link state.
    // The honest fix is an SPA-side rename to "Uplink"; www-spa is a sibling
    // repo, so that is raised rather than done here.
    d["wifi"] = net.link_up;

    // Ethernet-specific detail the S3 card has no row for yet, but which the
    // REST consumer and any future SPA revision can use.
    d["link_up"]     = net.link_up;
    d["link_speed"]  = net.speed_mbps;
    d["link_duplex"] = net.duplex_full ? "full" : "half";

    d["mqtt_connected"] = mqtt_gw_is_connected();
    d["ws_clients"]     = ws_server_client_count();

    // "P4 Sync" in the UI. On a two-chip build this tracks the SPI/HAP link.
    // Here the bridge is a set of direct calls in one process -- there is no
    // link that can fall out of sync, so it is always true.
    d["synced"] = true;
}

void sys_diag_push_tick() {
    JsonDocument doc;
    JsonObject d = doc.to<JsonObject>();
    sys_diag_fill(d, SYS_DIAG_CPU_TICK);
    ws_push("status.tick", doc);
}
