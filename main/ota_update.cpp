// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ota_update.cpp — see ota_update.h. Mirrors net-core's task_ota (S3 side of
// the dual-chip build), with target "self".
#include "ota_update.h"
#include "ota_redirect.h"

#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <strings.h>

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
#include "auth.h"
#include "radio_state.h"
#include "sys_state.h"
#include "rule_store.h"
#include "ws_bridge.h"
#include "log_cloud.h"
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
//   * events flowing     -- TaskEventBus started (rules, MQTT, UI depend on it)
//   * sign-in storage    -- the auth namespace opened (else the hub is locked)
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
    if (!sys_event_task_ok()) return "event dispatcher not running (rules and web UI would see no device events)";
    if (auth_storage_error()) return "sign-in storage not readable";
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
        // Only a saved verdict counts: if the boot metadata write fails the
        // bootloader still rolls back at the next reboot, so stay "pending"
        // and try again next tick (review F32).
        const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "could not mark the running image valid (%s) -- retrying", esp_err_to_name(e));
            return;
        }
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
    log_cloud_planned_restart(logcloud::Planned::Rollback);
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

// One request, headers only, redirects off: the HTTP status, and for a 3xx
// the absolute target (the client resolves a relative Location). Negative when
// the server cannot be reached.
int fetch_hop(const char* url, char* next, size_t cap) {
    esp_http_client_config_t c{};
    c.url = url;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.disable_auto_redirect = true;
    c.buffer_size_tx = 2048;   // a signed URL in the request line, as below
    esp_http_client_handle_t cl = esp_http_client_init(&c);
    if (!cl) return -1;
    int status = -1;
    if (esp_http_client_open(cl, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cl);
        status = esp_http_client_get_status_code(cl);
        if (status <= 0) status = -1;
        else if (ota_is_redirect(status) &&
                 (esp_http_client_set_redirection(cl) != ESP_OK ||
                  esp_http_client_get_url(cl, next, static_cast<int>(cap)) != ESP_OK))
            status = -1;
    }
    esp_http_client_cleanup(cl);
    return status;
}

// Set if the download itself meets a redirect after ota_resolve_redirects
// found none: the image is then not taken.
volatile bool s_late_redirect = false;
esp_err_t on_http_event(esp_http_client_event_t* e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && strcasecmp(e->header_key, "Location") == 0)
        s_late_redirect = true;
    return ESP_OK;
}

void run(const char* url) {
    event("ota.start", false, false, 0, 0, nullptr);
    // Where the image really is: every redirect on https://, a bounded number
    // of them (ota_redirect.h, review F09/F33). The download asks there directly.
    constexpr size_t kUrlMax = 1536;
    char* final_url = static_cast<char*>(malloc(2 * kUrlMax));
    if (!final_url) {
        event("ota.complete", true, false, 0, 0, "not enough memory to start the update");
        return;
    }
    const OtaResolve r = ota_resolve_redirects(url, final_url, final_url + kUrlMax, kUrlMax, fetch_hop);
    if (r != OtaResolve::OK) {
        const char* why = r == OtaResolve::INSECURE ? "the update server redirected to an insecure http:// address"
                        : r == OtaResolve::TOO_MANY ? "the update server redirected too many times"
                        : "the update server could not be reached";
        ESP_LOGE(TAG, "update refused: %s", why);
        event("ota.complete", true, false, 0, 0, why);
        free(final_url);
        return;
    }

    esp_http_client_config_t http{};
    http.url = final_url;
    http.crt_bundle_attach = esp_crt_bundle_attach;   // verify the server
    http.keep_alive_enable = true;
    // The signed URL GitHub redirects to is ~950 chars; the request line must
    // fit the TX buffer (default 512, "Out of buffer").
    http.buffer_size_tx = 2048;
    http.event_handler = on_http_event;
    esp_https_ota_config_t cfg{};
    cfg.http_config = &http;

    s_late_redirect = false;
    esp_https_ota_handle_t h = nullptr;
    esp_err_t ret = esp_https_ota_begin(&cfg, &h);
    free(final_url);                        // the client keeps its own copy
    if (ret == ESP_OK && h && s_late_redirect) {
        esp_https_ota_abort(h);
        ESP_LOGE(TAG, "update refused: the download was redirected again");
        event("ota.complete", true, false, 0, 0, "the update server redirected the download");
        return;
    }
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
    log_cloud_planned_restart(logcloud::Planned::Update);
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
