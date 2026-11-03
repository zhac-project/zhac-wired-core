// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ota_update.cpp — see ota_update.h. Mirrors net-core's task_ota (S3 side of
// the dual-chip build), with target "self".
#include "ota_update.h"
#include "ota_image.h"
#include "ota_redirect.h"

#include <atomic>
#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "ArduinoJson.h"
#include "esp_attr.h"      // __NOINIT_ATTR: the early-crash counter
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
#include "zap_tls.h"
#include "sdkconfig.h"

// zap_tls checks certificate dates only when mbedTLS has a date source.
// sdkconfig.defaults turns it on (a764fb8), but menuconfig or a custom
// sdkconfig can turn it off again, and check_resolved_config.sh runs only in
// CI and the build scripts. Fail every build instead of shipping an image
// that accepts expired certificates (review 2026-11-03 K-04).
#if !CONFIG_MBEDTLS_HAVE_TIME_DATE
#error "CONFIG_MBEDTLS_HAVE_TIME_DATE is off, so TLS would accept expired certificates: turn it back on (sdkconfig.defaults sets it)"
#endif

static const char* TAG = "ota";

namespace {

volatile bool     s_busy = false;
char              s_url[256];
bool              s_allow_older = false;   // set with s_url, read by TaskOTA

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
constexpr const char* kNvsStoreOk    = "ota_store_ok";   // u8: device store was fine before it (absent: yes)
constexpr const char* kNvsRollback   = "ota_rb_why";     // str: why the last update was undone

esp_timer_handle_t s_health_timer = nullptr;
uint32_t           s_health_ticks = 0;
// Review 2026-10-24 PB-04: one passing tick used to keep an image, so a crash
// a minute later could no longer roll it back. The checks must now pass on
// every tick for this long in a row (well inside kHealthDeadlineS).
constexpr uint32_t kSoakS            = 5 * 60;
uint32_t           s_healthy_ticks   = 0;
// Review 2026-10-28 R-03 (S1): the image is valid, but neither mark of the
// device store's move to zhac_data would write yet. Retried until one sticks;
// until then the old copy stays on nvs. A restart meanwhile loses nothing: a
// boot keeps the copy while nvs is unchanged (review 2026-10-29 N-05). Set on
// the boot that kept the image and on any later boot that still finds the move
// unmarked, with the store open (fix review M-3, fix re-review M-a). Shown in
// status; the older-version check asks the store itself.
std::atomic<bool>  s_move_unconfirmed{false};
// Each try logs its failed writes, so the tries back off: 5 s, then twice as
// far apart each time, up to once an hour (fix review M-4).
constexpr uint32_t kRetryMaxTicks    = 3600 / kHealthTickS;
uint32_t           s_retry_tick      = 0;   // health tick of the next try
uint32_t           s_retry_gap       = 1;   // ticks to the try after it
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
    uint8_t store_was_ok = 1;   // a build before this one did not record it: hold the update to it
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, kNvsRadioOk, &radio_was_ok);
        nvs_get_u8(h, kNvsStoreOk, &store_was_ok);
        nvs_close(h);
    }
    // Only a store that worked before: an update must not roll back for one
    // that was already shut (and may be what the update is meant to fix).
    if (store_was_ok && !zap_store_is_ready()) return "device store not readable (it was before the update)";
    if (radio_was_ok && !radio_ok()) return "Zigbee radio not ready (it was before the update)";
    return nullptr;
}

void health_tick(void*) {
    s_health_ticks++;
    if (s_move_unconfirmed) {
        if (s_health_ticks < s_retry_tick) return;
        s_retry_gap = s_retry_gap < kRetryMaxTicks / 2 ? s_retry_gap * 2 : kRetryMaxTicks;
        s_retry_tick = s_health_ticks + s_retry_gap;
        if (zap_store_confirm_move()) {
            s_move_unconfirmed = false;
            ESP_LOGI(TAG, "device store move to zhac_data confirmed");
            esp_timer_stop(s_health_timer);
        } else {
            ESP_LOGW(TAG, "device store move cannot be marked final yet (storage full?) -- "
                          "the old copy stays on nvs; next try in %" PRIu32 " s", s_retry_gap * kHealthTickS);
        }
        return;
    }
    const char* why = health_problem();
    s_healthy_ticks = why ? 0 : s_healthy_ticks + 1;
    if (!why && s_healthy_ticks * kHealthTickS < kSoakS) return;   // healthy, still soaking
    if (!why) {
        // Only a saved verdict counts: if the boot metadata write fails the
        // bootloader still rolls back at the next reboot, so stay "pending"
        // and try again next tick (review F32).
        const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "could not mark the running image valid (%s) -- retrying", esp_err_to_name(e));
            return;
        }
        ESP_LOGI(TAG, "running image marked valid after %" PRIu32 " s: storage, web server and radio OK "
                      "for %" PRIu32 " s in a row", s_health_ticks * kHealthTickS, kSoakS);
        // No rollback any more: the move of the device store to zhac_data made
        // on this trial boot becomes final, so the old copy on nvs can go.
        s_state = "verified";
        nvs_put_str(kNvsRollback, nullptr);
        if (zap_store_is_ready() && !zap_store_confirm_move()) {   // a shut store: see ota_update_init()
            s_move_unconfirmed = true;   // keep the timer: retried, backing off (above)
            ESP_LOGW(TAG, "device store move cannot be marked final yet -- retrying");
            return;
        }
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

struct Hop { char* loc; size_t cap; bool cut; };   // the Location a hop answered with

esp_err_t on_hop_event(esp_http_client_event_t* e) {
    auto* h = static_cast<Hop*>(e->user_data);
    if (h && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Location") == 0 &&
        snprintf(h->loc, h->cap, "%s", e->header_value) >= static_cast<int>(h->cap))
        h->cut = true;
    return ESP_OK;
}

// One request, headers only, redirects off: the HTTP status, and for a 3xx
// the Location value as sent (ota_follow resolves a relative one). Negative
// when the server cannot be reached or the Location does not fit. Not
// esp_http_client_get_url(): it leaves the query out, and a signed download
// URL carries its signature there (review 2026-10-24 F-04).
int fetch_hop(const char* url, char* next, size_t cap) {
    Hop hop{next, cap, false};
    next[0] = '\0';
    esp_http_client_config_t c{};
    c.url = url;
    c.crt_bundle_attach = zap_tls_attach;   // the CA bundle; dates once the clock is set (HS-11)
    c.disable_auto_redirect = true;
    c.buffer_size_tx = 2048;   // a signed URL in the request line, as below
    c.event_handler = on_hop_event;
    c.user_data = &hop;
    esp_http_client_handle_t cl = esp_http_client_init(&c);
    if (!cl) return -1;
    int status = -1;
    if (esp_http_client_open(cl, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cl);
        status = esp_http_client_get_status_code(cl);
        if (status <= 0 || (hop.cut && ota_is_redirect(status))) status = -1;
    }
    esp_http_client_cleanup(cl);
    return status;
}

// Set if the download itself meets a redirect after ota_resolve_redirects
// found none: the image is then not taken.
volatile bool s_late_redirect = false;
esp_err_t on_http_event(esp_http_client_event_t* e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && strcasecmp(e->header_key, "Location") == 0) {
        s_late_redirect = true;
    } else if (s_late_redirect &&
               (e->event_id == HTTP_EVENT_ON_CONNECTED || e->event_id == HTTP_EVENT_HEADERS_SENT)) {
        // esp_https_ota_begin is following that redirect: it follows every
        // one with no limit and returns only when an answer is not a redirect
        // (IDF esp_https_ota.c _http_connect), so a server could keep the
        // update task, and requests to whatever it names, going until the hub
        // restarts (review HS-09). Nothing can stop it from here but a
        // restart; nothing is written yet, and an update never starts while
        // the last one is on trial, so the running firmware stays.
        ESP_LOGE(TAG, "update refused: the download was redirected again -- restarting to end it");
        event("ota.complete", true, false, 0, 0, "the update server redirected the download; the hub restarts");
        vTaskDelay(pdMS_TO_TICKS(500));   // let the page get the answer
        log_cloud_planned_restart(logcloud::Planned::Restart);
        esp_restart();
    }
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
    http.crt_bundle_attach = zap_tls_attach;   // verify the server (zap_tls.h)
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
    // Only this firmware, and no older release unless the owner asked for one
    // (ota_image.h, review 2026-10-24 HS-10). The image's strings are its own:
    // copied with a terminator, control bytes as '?' (they go into the log and
    // the ota.complete event).
    esp_app_desc_t img{};
    if (esp_https_ota_get_img_desc(h, &img) != ESP_OK) {   // a short read too: the link dropped
        esp_https_ota_abort(h);
        ESP_LOGE(TAG, "update refused: no app description in the download");
        event("ota.complete", true, false, 0, 0, "the download is not a firmware image, or it broke off");
        return;
    }
    char proj[sizeof(img.project_name) + 1] = {};
    char ver[sizeof(img.version) + 1] = {};
    memcpy(proj, img.project_name, sizeof(img.project_name));
    memcpy(ver, img.version, sizeof(img.version));
    for (char* s : {proj, ver})
        for (; *s; ++s)
            if (static_cast<unsigned char>(*s) < 0x20 || *s == 0x7F) *s = '?';
    const esp_app_desc_t* me = esp_app_get_description();
    const OtaImage verdict = ota_image_check(me->project_name, me->version, proj, ver, s_allow_older);
    if (verdict != OtaImage::OK) {
        char why[160];
        if (verdict == OtaImage::OTHER_PROJECT)
            snprintf(why, sizeof(why), "the image is %s, not the %s firmware", proj, me->project_name);
        else
            snprintf(why, sizeof(why), "the image is an older release (%s) than the one running (%s)", ver, me->version);
        esp_https_ota_abort(h);
        ESP_LOGE(TAG, "update refused: %s", why);
        event("ota.complete", true, false, 0, 0, why);
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
    nvs_put_u8(kNvsStoreOk, zap_store_is_ready() ? 1 : 0);
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

// ── Early-crash guard (review 2026-10-24 PB-04) ───────────────────────────
// The bootloader rolls back only an image on trial, so a kept image that
// crashes soon after every boot loops for ever. Three boots in a row that end
// in a crash within kEarlyUs of starting roll back to the other slot when it
// holds a bootable image; otherwise the hub runs in safe mode (no Lua, MQTT or
// cloud link: the web UI stays up, the radio keeps its own guard) until a boot
// lasts kEarlyUs. No-init RAM: it survives the resets it counts.
constexpr uint32_t kEarlyMagic   = 0x5AFE0B07;
constexpr int64_t  kEarlyUs      = 2 * 60 * 1000000LL;
constexpr uint32_t kEarlyCrashes = 3;
__NOINIT_ATTR uint32_t s_early_magic;
__NOINIT_ATTR uint32_t s_early_armed;   // 1 from boot until kEarlyUs of uptime
__NOINIT_ATTR uint32_t s_early_count;   // boots in a row that crashed early
bool s_safe_mode = false;

bool crash_reset(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_CPU_LOCKUP;
}

}  // namespace

bool ota_update_boot_guard() {
    const esp_reset_reason_t why = esp_reset_reason();
    if (s_early_magic != kEarlyMagic || why == ESP_RST_POWERON) {
        s_early_magic = kEarlyMagic;
        s_early_count = 0;
    } else if (s_early_armed == 1 && crash_reset(why)) {
        s_early_count++;
    } else {
        s_early_count = 0;
    }
    s_early_armed = 1;
    static esp_timer_handle_t disarm = nullptr;
    esp_timer_create_args_t a{};
    a.callback = [](void*) { s_early_armed = 0; s_early_count = 0; };   // this boot lasted
    a.name = "early_guard";
    if (!disarm && esp_timer_create(&a, &disarm) == ESP_OK) esp_timer_start_once(disarm, kEarlyUs);
    const uint32_t n = s_early_count;
    if (n < kEarlyCrashes) return false;
    if (esp_ota_check_rollback_is_possible()) {
        ESP_LOGE(TAG, "%" PRIu32 " boots in a row crashed within %d s -- rolling back to the other image",
                 n, static_cast<int>(kEarlyUs / 1000000));
        s_early_count = 0;
        nvs_put_str(kNvsRollback, "it crashed three times in a row within two minutes of starting");
        log_cloud_planned_restart(logcloud::Planned::Rollback);
        esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only if it could not
    }
    s_safe_mode = true;
    ESP_LOGE(TAG, "%" PRIu32 " boots in a row crashed within %d s -- SAFE MODE: Lua, MQTT and the cloud link "
                  "stay off this boot", n, static_cast<int>(kEarlyUs / 1000000));
    return true;
}

bool ota_update_safe_mode() { return s_safe_mode; }

static void start_health_timer() {
    esp_timer_create_args_t a{};
    a.callback = health_tick;
    a.name = "ota_health";
    if (esp_timer_create(&a, &s_health_timer) == ESP_OK)
        esp_timer_start_periodic(s_health_timer, static_cast<uint64_t>(kHealthTickS) * 1000000ULL);
}

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
        // A move of the device store left unmarked at an earlier boot (or this
        // one) is not final yet: keep trying, as after a trial (fix review M-3).
        // Not while the store is shut: nothing can be marked before the next
        // boot, and status already says storage_error (fix re-review M-a).
        if (zap_store_is_ready() && !zap_store_confirm_move()) {
            s_move_unconfirmed = true;
            start_health_timer();
        }
        return;
    }
    s_state = "pending";
    ESP_LOGI(TAG, "new image on trial: kept once storage, web server and radio check out (%" PRIu32 " s limit)",
             kHealthDeadlineS);
    start_health_timer();
}

const char* ota_update_state() { return s_state; }

const char* ota_update_rollback_reason() { return s_rollback_why[0] ? s_rollback_why : nullptr; }

bool ota_update_store_unconfirmed() { return s_move_unconfirmed; }

bool ota_update_rolled_back() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0;   // nvs_put_str erases the key for "", so present = set
    const bool set = nvs_get_str(h, kNvsRollback, nullptr, &n) == ESP_OK && n > 1;
    nvs_close(h);
    return set;
}

bool ota_update_start(const char* url, bool allow_older, const char** err) {
    if (!url || strncmp(url, "https://", 8) != 0) { *err = "https:// URL required"; return false; }
    if (strlen(url) >= sizeof(s_url)) { *err = "URL too long"; return false; }
    if (s_busy) { *err = "an update is already running"; return false; }
    if (s_state[0] == 'p') { *err = "the previous update is still being checked -- wait a few minutes"; return false; }
    // An older build would run on the copy on nvs, from before the last update,
    // and the next update would copy it over what changed since (fix review M-1).
    // A newer build keeps the copy. Asked of the store rather than the flag, so
    // no moment of a boot is left out (fix re-review N3). A boot-guard rollback
    // (three early crashes) cannot be refused: once the older build writes its
    // store, the next update copies that over what was saved since.
    if (allow_older && !zap_store_move_final()) {
        *err = "an older version cannot be installed until the device store's move is final -- see the log";
        return false;
    }
    s_busy = true;
    snprintf(s_url, sizeof(s_url), "%s", url);
    s_allow_older = allow_older;
    if (xTaskCreate(task, "TaskOTA", 8192, nullptr, 2, nullptr) != pdPASS) {
        s_busy = false;
        *err = "not enough memory to start the update";
        return false;
    }
    return true;
}
