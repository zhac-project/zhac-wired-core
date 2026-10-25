// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// supervisor.cpp -- see supervisor.h.
#include "supervisor.h"

#include <atomic>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_zigbee_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zhac_task.h"

static const char* TAG = "supervisor";

namespace {

constexpr int64_t  kStallUs   = 3 * 60 * 1000000LL;   // silent this long: restart
constexpr uint32_t kTickMs    = 5000;                 // also feeds the task watchdog (10 s)
constexpr uint32_t kProbeEvery = 6;                   // web-server probe every 30 s

httpd_handle_t         s_hd = nullptr;
std::atomic<int64_t>   s_httpd_seen_us{0};   // last time a queued probe ran on the httpd task

// Not a planned restart: the next boot reports an unexpected end, and this line
// survives in the log ring. A shutdown handler blocked on a lock the stalled
// task holds is caught by the task watchdog (this task is subscribed).
void stalled(const char* what) {
    ESP_LOGE(TAG, "%s has not answered for %d s -- restarting the hub", what,
             static_cast<int>(kStallUs / 1000000));
    vTaskDelay(pdMS_TO_TICKS(200));   // let the line reach the log
    esp_restart();
}

void task_supervisor(void*) {
    esp_task_wdt_add(nullptr);
    s_httpd_seen_us.store(esp_timer_get_time());
    for (uint32_t tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
        esp_task_wdt_reset();
        const int64_t now = esp_timer_get_time();
        if (s_hd && tick % kProbeEvery == 0) {
            httpd_queue_work(s_hd, [](void*) { s_httpd_seen_us.store(esp_timer_get_time()); }, nullptr);
        }
        if (s_hd && now - s_httpd_seen_us.load() > kStallUs) stalled("the web server task");
        // 0 while the radio is not running (radio-less boot, no backend): nothing to watch.
        // ponytail: a radio that wedges again after every start restarts the hub every ~3 min;
        // count these restarts in no-init RAM and stop after a few if that is ever seen.
        const int64_t zb = esp_zigbee_backend_heartbeat_us();
        if (zb && now - zb > kStallUs) stalled("the Zigbee stack");
    }
}

}  // namespace

void supervisor_start(httpd_handle_t hd) {
    static bool started = false;
    if (started) return;
    started = true;
    s_hd = hd;
    // 4 KB: esp_restart() runs the store flush handlers on this stack.
    if (zhac_task_create(task_supervisor, "TaskSupervisor", 4096, nullptr, 1, nullptr) != pdPASS)
        ESP_LOGE(TAG, "supervisor task not started");
}
