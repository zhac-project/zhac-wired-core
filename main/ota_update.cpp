// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ota_update.cpp — see ota_update.h. Mirrors net-core's task_ota (S3 side of
// the dual-chip build), with target "self".
#include "ota_update.h"

#include <cstdio>
#include <cinttypes>
#include <cstring>

#include "ArduinoJson.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_state.h"
#include "rule_store.h"
#include "ws_bridge.h"
#include "ws_server.h"
#include "zap_store.h"

static const char* TAG = "ota";

namespace {

volatile bool     s_busy = false;
char              s_url[256];

// ── Post-update health ──────────────────────────────────────────────────────
// A fresh image is kept only once it has shown it can do the hub's job, not
// merely because it stayed up for a minute:
//   * storage readable   -- NVS answers (devices, rules, names live there)
//   * web UI reachable   -- the HTTP server that serves it is up
//   * radio no worse     -- if the radio worked before the update, it works now
// Ethernet is deliberately NOT a criterion: an unplugged cable or a router
// reboot must never roll a good firmware back. Unmet after kHealthDeadlineS,
// the image is marked invalid and the bootloader boots the previous one; the
// reason is kept in NVS so the old firmware can show it in status.
constexpr uint32_t kHealthTickS      = 5;
constexpr uint32_t kHealthDeadlineS  = 10 * 60;
constexpr const char* kNvsNs         = "sys_cfg";
constexpr const char* kNvsRadioOk    = "ota_radio_ok";   // u8: radio was fine before the update
constexpr const char* kNvsRollback   = "ota_rb_why";     // str: why the last update was undone

esp_timer_handle_t s_health_timer = nullptr;
uint32_t           s_health_ticks = 0;
const char*        s_state        = "verified";          // "pending" | "verified" | "rolling_back"
char               s_rollback_why[64] = {};

void nvs_put_u8(const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void nvs_put_str(const char* key, const char* v) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (v && *v) nvs_set_str(h, key, v); else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

// The first unmet criterion, or nullptr when the image is healthy.
const char* health_problem() {
    nvs_stats_t st{};
    if (nvs_get_stats(nullptr, &st) != ESP_OK) return "storage (NVS) not readable";
    if (!ws_server_get_handle()) return "web server not running";
    uint8_t radio_was_ok = 0;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, kNvsRadioOk, &radio_was_ok);
        nvs_close(h);
    }
    if (radio_was_ok && !radio_ok()) return "Zigbee radio not ready (it was before the update)";
    return nullptr;
}

void health_tick(void*) {
    s_health_ticks++;
    const char* why = health_problem();
    if (!why) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "running image marked valid after %" PRIu32 " s: storage, web server and radio OK",
                     s_health_ticks * kHealthTickS);
        s_state = "verified";
        nvs_put_str(kNvsRollback, nullptr);
        esp_timer_stop(s_health_timer);
        return;
    }
    if (s_health_ticks * kHealthTickS < kHealthDeadlineS) {
        if (s_health_ticks % 12 == 1)   // once a minute
            ESP_LOGW(TAG, "new image not yet healthy: %s (%" PRIu32 " s of %" PRIu32 ")",
                     why, s_health_ticks * kHealthTickS, kHealthDeadlineS);
        return;
    }
    ESP_LOGE(TAG, "new image unhealthy after %" PRIu32 " s: %s -- rolling back", kHealthDeadlineS, why);
    s_state = "rolling_back";
    nvs_put_str(kNvsRollback, why);
    esp_timer_stop(s_health_timer);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void event(const char* name, bool has_ok, bool ok, int offset, int total, const char* err) {
    JsonDocument d;
    d["target"] = "self";
    if (has_ok) d["ok"] = ok;
    d["offset"] = offset;
    d["total"]  = total;
    if (total > 0) d["pct"] = offset * 100 / total;
    if (err) d["err"] = err;
    ws_push(name, d);
}

void run(const char* url) {
    esp_http_client_config_t http{};
    http.url = url;
    http.crt_bundle_attach = esp_crt_bundle_attach;   // verify the server
    http.keep_alive_enable = true;
    http.max_redirection_count = 5;                   // GitHub release assets redirect
    esp_https_ota_config_t cfg{};
    cfg.http_config = &http;

    event("ota.start", false, false, 0, 0, nullptr);
    esp_https_ota_handle_t h = nullptr;
    esp_err_t ret = esp_https_ota_begin(&cfg, &h);
    if (ret != ESP_OK || !h) {
        ESP_LOGE(TAG, "begin failed: %s", esp_err_to_name(ret));
        event("ota.complete", true, false, 0, 0, esp_err_to_name(ret));
        return;
    }
    int total = esp_https_ota_get_image_size(h);
    if (total < 0) total = 0;
    int last_pct = -1;
    while ((ret = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int got = esp_https_ota_get_image_len_read(h);
        const int pct = total > 0 ? got * 100 / total : 0;
        if (pct != last_pct) {
            event("ota.progress", false, false, got, total, nullptr);
            last_pct = pct;
        }
    }
    if (ret == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        ret = esp_https_ota_finish(h);      // validates the image, switches the boot slot
    } else {
        esp_https_ota_abort(h);
        if (ret == ESP_OK) ret = ESP_FAIL;  // truncated download
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(ret));
        event("ota.complete", true, false, 0, total, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "update written -- rebooting in 2 s");
    event("ota.complete", true, true, total, total, nullptr);
    // What the new image has to live up to, and a clean slate for its verdict.
    nvs_put_u8(kNvsRadioOk, radio_ok() ? 1 : 0);
    nvs_put_str(kNvsRollback, nullptr);
    rule_store_flush_now();
    zap_store_flush_now();
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

// One task per update, created on demand: esp_https_ota writes flash, so its
// stack must be internal RAM -- the scarce kind -- and it is only held while an
// update runs.
void task(void*) {
    run(s_url);          // reboots on success
    s_busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

void ota_update_init() {
    if (s_health_timer) return;
    // Why the previous update was undone, if it was: the old image boots with
    // this in NVS and shows it in status until the next update starts.
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_rollback_why);
        if (nvs_get_str(h, kNvsRollback, s_rollback_why, &n) != ESP_OK) s_rollback_why[0] = '\0';
        nvs_close(h);
    }
    if (s_rollback_why[0]) ESP_LOGW(TAG, "the last update was rolled back: %s", s_rollback_why);

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        s_state = "verified";     // USB-flashed, or already judged
        return;
    }
    s_state = "pending";
    ESP_LOGI(TAG, "new image on trial: kept once storage, web server and radio check out (%" PRIu32 " s limit)",
             kHealthDeadlineS);
    esp_timer_create_args_t a{};
    a.callback = health_tick;
    a.name = "ota_health";
    if (esp_timer_create(&a, &s_health_timer) == ESP_OK)
        esp_timer_start_periodic(s_health_timer, static_cast<uint64_t>(kHealthTickS) * 1000000ULL);
}

const char* ota_update_state() { return s_state; }

const char* ota_update_rollback_reason() { return s_rollback_why[0] ? s_rollback_why : nullptr; }

bool ota_update_start(const char* url, const char** err) {
    if (!url || strncmp(url, "https://", 8) != 0) { *err = "https:// URL required"; return false; }
    if (strlen(url) >= sizeof(s_url)) { *err = "URL too long"; return false; }
    if (s_busy) { *err = "an update is already running"; return false; }
    if (s_state[0] == 'p') { *err = "the previous update is still being checked -- wait a few minutes"; return false; }
    s_busy = true;
    snprintf(s_url, sizeof(s_url), "%s", url);
    if (xTaskCreate(task, "TaskOTA", 8192, nullptr, 2, nullptr) != pdPASS) {
        s_busy = false;
        *err = "not enough memory to start the update";
        return false;
    }
    return true;
}
