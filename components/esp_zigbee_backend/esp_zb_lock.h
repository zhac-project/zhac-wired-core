// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_lock.h -- the esp-zigbee SDK lock, made safe to take from anywhere.
//
// The lib (esp_zigbee.h, esp_zigbee_lock_acquire) makes the lock mandatory
// around every SDK call that is not made from inside a stack callback. Until
// review 2026-09 (WC-01) this backend never took it: ezb_* requests were
// issued from httpd, the interview task, the configure worker, the event-bus
// task and the main task straight into a running mainloop.
//
// Two things make the raw API awkward here, so this header wraps it:
//   * callbacks (on_apsde_indication, on_app_signal, ZDO/bind results) run on
//     the stack task with the lock already held by the mainloop, so a guard
//     taken there must be a no-op. task_zigbee records its handle for that.
//   * several paths nest (zb_start_discovery -> zb_network_ready -> getters),
//     so the guard is re-entrant per task through an owner/depth pair kept
//     here, independent of whether the lib's mutex happens to be recursive.
// Acquisition is bounded: a stack task wedged inside a callback must surface
// as one failed request, not as a wedged httpd worker holding the SPA.
//
// The owner/depth pair is only ever written by the task that holds the lock
// (set right after acquire, cleared right before release), and a reader
// compares it against its own handle, so a stale read can never equal the
// reader. No extra mutex needed.
#pragma once

#include <cstdint>

#include "esp_log.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace zb_lock {

inline constexpr uint32_t kTimeoutMs = 2000;

inline TaskHandle_t& stack_task() { static TaskHandle_t h = nullptr; return h; }
inline TaskHandle_t& owner()      { static TaskHandle_t h = nullptr; return h; }
inline uint32_t&     depth()      { static uint32_t     d = 0;       return d; }

// Called once from task_zigbee, before esp_zigbee_launch_mainloop().
inline void set_stack_task(TaskHandle_t h) { stack_task() = h; }

// RAII guard. `if (!g) return false;` after construction is the contract:
// a false guard means the SDK must not be called on this path.
struct Guard {
    bool ok = false;

    explicit Guard(uint32_t timeout_ms = kTimeoutMs) {
        const TaskHandle_t me = xTaskGetCurrentTaskHandle();
        if (me == stack_task()) { ok = true; return; }          // callback context
        if (owner() == me)      { ++depth(); ok = true; return; } // re-entrant
        if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(timeout_ms))) {
            ESP_LOGW("zb_lock", "SDK lock not acquired within %u ms (task %s)",
                     (unsigned)timeout_ms, pcTaskGetName(me));
            return;
        }
        owner() = me;
        depth() = 1;
        ok = true;
    }
    ~Guard() {
        if (!ok) return;
        const TaskHandle_t me = xTaskGetCurrentTaskHandle();
        if (me == stack_task()) return;
        if (owner() != me) return;            // cannot happen; defensive
        if (--depth() == 0) {
            owner() = nullptr;
            esp_zigbee_lock_release();
        }
    }
    explicit operator bool() const { return ok; }
    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;
};

}  // namespace zb_lock
