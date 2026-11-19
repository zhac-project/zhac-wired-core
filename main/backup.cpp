// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Full backup and restore (research 2026-11-18 C1): backup_core.h on esp_partition and the web server.
//
//   POST /api/backup/full     {"password":"..."} -> the .zhacbak file (application/octet-stream)
//   POST /api/backup/restore  application/octet-stream: u16 LE password length, the password, the file
//                             -> {"ok":true}; the hub restarts and backup_apply_pending() writes it over the storage
//
// Both run on the web server's task, for seconds (the key derivation alone takes about two): its other requests and
// the WebSocket wait meanwhile, and the supervisor allows that task three minutes. Design and file format:
// extra/docs/superpowers/specs/2026-11-19-full-backup-design.md, main/backup_format.h.
#include "backup.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <strings.h>
#include <sys/socket.h>

#include "ArduinoJson.h"
#include "auth.h"
#include "backup_core.h"
#include "device_shadow.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_body.h"
#include "log_cloud.h"
#include "net_discovery.h"
#include "ota_update.h"
#include "rule_store.h"
#include "sdkconfig.h"
#include "zap_store.h"

using namespace backup;

namespace {

const char* TAG = "backup";
constexpr time_t kClockSet = 1700000000;   // as backup_format.h: a clock below this was never set

// A partition through esp_partition. Erases and writes give up the CPU for a tick every 50 ms: a restore never
// waits on the network (the flash is the slower side), and for half a minute IDLE1 would starve into the task
// watchdog.
struct PartFlash : Flash {
    const esp_partition_t* p = nullptr;
    int64_t breath = 0;
    uint32_t size() const override { return p->size; }
    bool read(uint32_t off, void* d, size_t n) override { return esp_partition_read(p, off, d, n) == ESP_OK; }
    bool write(uint32_t off, const void* s, size_t n) override {
        breathe();
        return esp_partition_write(p, off, s, n) == ESP_OK;
    }
    bool erase(uint32_t off, size_t n) override {
        breathe();
        return esp_partition_erase_range(p, off, n) == ESP_OK;
    }
    void breathe() {
        if (esp_timer_get_time() - breath < 50000) return;
        vTaskDelay(1);
        breath = esp_timer_get_time();
    }
};

// This hub's storage partitions, in kPartNames order. zhac_data can be missing (main.cpp registers it).
struct Storage {
    PartFlash part[kMaxParts];
    Part list[kMaxParts] = {};   // the ones present, for backup_format.h
    size_t n = 0;
    Storage() {
        for (size_t i = 0; i < kMaxParts; i++) {
            part[i].p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartNames[i]);
            if (!part[i].p) continue;
            snprintf(list[n].name, sizeof list[n].name, "%s", kPartNames[i]);
            list[n++].size = part[i].p->size;
        }
    }
    bool all() const { return n == kMaxParts; }
    static Flash* find(const char* name, void* ctx) {
        Storage* s = static_cast<Storage*>(ctx);
        for (size_t i = 0; i < kMaxParts; i++) {
            if (s->part[i].p && strcmp(kPartNames[i], name) == 0) return &s->part[i];
        }
        return nullptr;
    }
};

char s_outcome[192];   // what backup_apply_pending() did, logged again by backup_log_outcome()

esp_err_t reply(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t reply_err(httpd_req_t* req, Err e) {
    switch (e) {
    case Err::Format:   return reply(req, "400 Bad Request", "{\"error\":\"not_a_backup\"}");
    case Err::Password: return reply(req, "400 Bad Request", "{\"error\":\"wrong_password\"}");
    case Err::Length:   return reply(req, "400 Bad Request", "{\"error\":\"incomplete\"}");
    case Err::Damaged:  return reply(req, "400 Bad Request", "{\"error\":\"damaged\"}");
    case Err::Busy:     return reply(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
    case Err::Space:    return reply(req, "500 Internal Server Error", "{\"error\":\"no_space\"}");
    case Err::Flash:    return reply(req, "500 Internal Server Error", "{\"error\":\"flash\"}");
    default:            return reply(req, "500 Internal Server Error", "{\"error\":\"crypto\"}");
    }
}

bool fail(httpd_req_t* req, const char* status, const char* body) {
    reply(req, status, body);
    return false;
}

bool fail(httpd_req_t* req, Err e) {
    reply_err(req, e);
    return false;
}

// Must be this type: a page of another site cannot send it (or JSON) without a CORS preflight the hub never grants.
bool content_type_is(httpd_req_t* req, const char* type) {
    char ct[48] = {};
    return httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) == ESP_OK &&
           strncasecmp(ct, type, strlen(type)) == 0;
}

// Exactly n bytes of the body; false when the client stops sending (closes, or times out three times running).
bool recv_exact(httpd_req_t* req, void* dst, size_t n) {
    char* p = static_cast<char*>(dst);
    int timeouts = 0;
    while (n) {
        const int r = httpd_req_recv(req, p, n);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (r <= 0) return false;
        timeouts = 0;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// The download. Its headers go out with the first piece, so an error found before it (the snapshot) still gets a
// JSON reply.
struct Chunks : Sink {
    httpd_req_t* req;
    const char* disposition;
    size_t bytes = 0;
    Chunks(httpd_req_t* r, const char* d) : req(r), disposition(d) {}
    bool put(const uint8_t* p, size_t n) override {
        if (!bytes) {
            httpd_resp_set_type(req, "application/octet-stream");
            httpd_resp_set_hdr(req, "Content-Disposition", disposition);
        }
        bytes += n;
        return httpd_resp_send_chunk(req, reinterpret_cast<const char*>(p), n) == ESP_OK;
    }
};

void pause_between_tries() { vTaskDelay(pdMS_TO_TICKS(50)); }

esp_err_t send_backup(httpd_req_t* req, Storage& hub, const char* pw, size_t pw_len, uint8_t* const* bufs,
                      uint8_t* io) {
    rule_store_flush_now();
    device_shadow_flush_now();
    zap_store_flush_now();
    Header h{};
    h.n_parts = kMaxParts;
    h.kdf_iter = kKdfIter;
    esp_fill_random(h.salt, sizeof h.salt);
    esp_fill_random(h.nonce, sizeof h.nonce);
    const time_t now = time(nullptr);
    h.created = now >= kClockSet ? static_cast<uint32_t>(now) : 0;
    h.nwk_fc = esp_zigbee_backend_nwk_frame_counter();
    snprintf(h.chip, sizeof h.chip, "%s", CONFIG_IDF_TARGET);
    snprintf(h.fw, sizeof h.fw, "%s", esp_app_get_description()->version);
    const char* host = net_discovery_hostname();
    snprintf(h.host, sizeof h.host, "%s", host && *host ? host : "zhac");
    Flash* parts[kMaxParts];
    for (size_t i = 0; i < kMaxParts; i++) {
        h.parts[i] = hub.list[i];
        parts[i] = &hub.part[i];
    }
    char date[16] = "";
    if (h.created) {
        const time_t c = h.created;
        struct tm t;
        gmtime_r(&c, &t);
        strftime(date, sizeof date, "-%Y%m%d", &t);
    }
    char disposition[96];
    snprintf(disposition, sizeof disposition, "attachment; filename=\"zhac-full-%s%s.zhacbak\"", h.host, date);
    Chunks out(req, disposition);
    const int64_t t0 = esp_timer_get_time();
    const Err e = write_file(h, pw, pw_len, parts, bufs, out, io, pause_between_tries);
    if (e == Err::None) {
        ESP_LOGI(TAG, "full backup sent: %u bytes in %lld ms", static_cast<unsigned>(out.bytes),
                 static_cast<long long>((esp_timer_get_time() - t0) / 1000));
        return httpd_resp_send_chunk(req, nullptr, 0);
    }
    ESP_LOGE(TAG, "full backup failed (%d) after %u bytes", static_cast<int>(e), static_cast<unsigned>(out.bytes));
    if (out.bytes) return ESP_FAIL;   // cut off: the file is short and its tag never comes
    return reply_err(req, e);
}

esp_err_t handle_full(httpd_req_t* req) {
    if (!content_type_is(req, "application/json")) {
        return reply(req, "415 Unsupported Media Type", "{\"error\":\"json_required\"}");
    }
    char body[256];
    JsonDocument doc;
    if (http_read_body(req, body, sizeof body) < 0 || deserializeJson(doc, body)) {
        return reply(req, "400 Bad Request", "{\"error\":\"invalid request\"}");
    }
    const char* pw = doc["password"] | "";
    const size_t pw_len = strlen(pw);
    if (pw_len < kPwMin || pw_len > kPwMax) return reply(req, "400 Bad Request", "{\"error\":\"password_length\"}");
    Storage hub;
    if (!hub.all()) return reply(req, "500 Internal Server Error", "{\"error\":\"partition_missing\"}");
    if (!crypto_init()) return reply_err(req, Err::Crypto);

    // One snapshot of all four partitions (3.2 MB) in PSRAM, so they are all of one instant.
    uint8_t* bufs[kMaxParts] = {};
    uint8_t* io = static_cast<uint8_t*>(heap_caps_malloc(kPiece + 16, MALLOC_CAP_8BIT));
    bool mem = io != nullptr;
    for (size_t i = 0; i < kMaxParts; i++) {
        bufs[i] = static_cast<uint8_t*>(heap_caps_malloc(hub.list[i].size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        mem = mem && bufs[i];
    }
    const esp_err_t r = mem ? send_backup(req, hub, pw, pw_len, bufs, io)
                            : reply(req, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    for (uint8_t* b : bufs) heap_caps_free(b);
    heap_caps_free(io);
    return r;
}

// As ws_bridge's system.restart: off the web server's task once the reply is out; 6 KB because esp_restart() runs
// the store flushes on this stack.
void restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Replies; true once the restore is staged and the restart is on its way.
bool stage_restore(httpd_req_t* req) {
    uint8_t len[2];
    if (req->content_len < 2 + kPwMin + kHeaderLen + kTagLen || !recv_exact(req, len, sizeof len)) {
        return fail(req, "400 Bad Request", "{\"error\":\"invalid request\"}");
    }
    const size_t pw_len = len[0] | len[1] << 8;
    if (pw_len < kPwMin || pw_len > kPwMax) return fail(req, "400 Bad Request", "{\"error\":\"password_length\"}");
    char pw[kPwMax + 1];
    uint8_t hdr[kHeaderLen];
    if (!recv_exact(req, pw, pw_len) || !recv_exact(req, hdr, kHeaderLen)) return fail(req, Err::Length);
    pw[pw_len] = '\0';

    Header h{};
    Storage hub;
    const char* why = nullptr;
    char msg[96];
    switch (check(hdr, &h, hub.list, hub.n, esp_app_get_description()->version, &why)) {
    case Err::None: break;
    case Err::Parts:
        snprintf(msg, sizeof msg, "{\"error\":\"partitions_differ\",\"partition\":\"%s\"}", why ? why : "");
        return fail(req, "409 Conflict", msg);
    case Err::Newer:
        snprintf(msg, sizeof msg, "{\"error\":\"newer_firmware\",\"fw\":\"%s\"}", h.fw);
        return fail(req, "409 Conflict", msg);
    default: return fail(req, Err::Format);
    }
    if (req->content_len != 2 + pw_len + kHeaderLen + total(h) + kTagLen) return fail(req, Err::Length);
    if (!crypto_init()) return fail(req, Err::Crypto);
    uint8_t key[32];
    Err e = unlock(h, pw, pw_len, key);
    memset(pw, 0, sizeof pw);
    if (e != Err::None) return fail(req, e);

    const esp_partition_t* idle = esp_ota_get_next_update_partition(nullptr);
    std::unique_ptr<Stager> st(new (std::nothrow) Stager);
    std::unique_ptr<uint8_t[]> ct(new (std::nothrow) uint8_t[kPiece]);
    if (!idle || !st || !ct) {
        memset(key, 0, sizeof key);
        return fail(req, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    }
    // The bootloader must never pick the slot about to hold the backup. ESP_FAIL: it had no valid entry.
    esp_ota_invalidate_inactive_ota_data_slot();
    PartFlash slot;
    slot.p = idle;
    e = st->begin(slot, h, hdr, key);
    memset(key, 0, sizeof key);
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t left = total(h); e == Err::None && left;) {
        const size_t n = left < kPiece ? left : kPiece;
        e = recv_exact(req, ct.get(), n) ? st->feed(ct.get(), n) : Err::Length;
        left -= static_cast<uint32_t>(n);
    }
    uint8_t tag[kTagLen];
    if (e == Err::None) e = recv_exact(req, tag, sizeof tag) ? Err::None : Err::Length;
    if (e == Err::None) {
        const uint32_t fc = fc_target(h.nwk_fc, h.created, static_cast<uint32_t>(time(nullptr)),
                                      esp_zigbee_backend_nwk_frame_counter());
        e = st->finish(tag, fc);
    }
    if (e != Err::None) {
        ESP_LOGE(TAG, "full restore not staged (%d)", static_cast<int>(e));
        return fail(req, e);
    }
    ESP_LOGW(TAG, "full restore staged in %lld ms (backup of %s, %s): restarting to apply it",
             static_cast<long long>((esp_timer_get_time() - t0) / 1000), h.host, h.fw);
    log_cloud_planned_restart(logcloud::Planned::Restart);
    reply(req, "200 OK", "{\"ok\":true}");
    if (xTaskCreate(restart_task, "restart", 6144, nullptr, 2, nullptr) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return true;
}

// Replies; true once the restore is staged and the restart is on its way.
bool restore(httpd_req_t* req) {
    if (!content_type_is(req, "application/octet-stream")) {
        return fail(req, "415 Unsupported Media Type", "{\"error\":\"octet_stream_required\"}");
    }
    if (ota_update_state()[0] == 'p') return fail(req, "409 Conflict", "{\"error\":\"on_trial\"}");
    if (!ota_update_slot_claim()) return fail(req, "409 Conflict", "{\"error\":\"update_running\"}");
    // Staged: the slot stays claimed until the restart, so no update erases the backup first.
    if (stage_restore(req)) return true;
    ota_update_slot_release();
    return false;
}

// ws_server fails a request's reads 8 s after its first byte until the hub answers (its slowloris guard); a restore
// answers only once it is staged, 20-30 s in. This request reads through its own function instead, with one limit
// for the whole upload, inside the supervisor's 3 minutes for the web server's task (review P5 C1).
constexpr int64_t kRestoreBudgetUs = 120 * 1000 * 1000;
int64_t s_restore_until;   // the web server has one task: one restore at a time

int restore_recv(httpd_handle_t, int fd, char* buf, size_t len, int flags) {
    if (!buf) return HTTPD_SOCK_ERR_INVALID;
    if (esp_timer_get_time() > s_restore_until) return HTTPD_SOCK_ERR_FAIL;
    const int r = recv(fd, buf, len, flags);
    if (r >= 0) return r;
    return errno == EAGAIN || errno == EINTR ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
}

esp_err_t handle_restore(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    s_restore_until = esp_timer_get_time() + kRestoreBudgetUs;
    httpd_sess_set_recv_override(req->handle, fd, restore_recv);
    // Any answer but the restart: httpd reads the rest of the body, then closes this session, which is no longer
    // under ws_server's guard.
    if (!restore(req)) httpd_sess_trigger_close(req->handle, fd);
    return ESP_OK;
}

}  // namespace

uint32_t backup_apply_pending() {
    const esp_partition_t* idle = esp_ota_get_next_update_partition(nullptr);
    char magic[8];
    if (!idle || esp_partition_read(idle, 0, magic, sizeof magic) != ESP_OK || memcmp(magic, "ZHACRS01", 8) != 0) {
        return 0;
    }
    PartFlash slot;
    slot.p = idle;
    Storage hub;
    std::unique_ptr<uint8_t[]> tmp(new (std::nothrow) uint8_t[kPiece]);
    if (!tmp) {
        ESP_LOGE(TAG, "a full restore is staged but there is no memory to apply it: next boot");
        return 0;
    }
    Stage st{};
    switch (apply(slot, Storage::find, &hub, tmp.get(), &st)) {
    case Applied::Nothing: return 0;
    case Applied::AlreadyDone: return st.fc_target;
    case Applied::Done:
        snprintf(s_outcome, sizeof s_outcome, "storage restored from the full backup of %s (%s)", st.host, st.fw);
        ESP_LOGW(TAG, "%s", s_outcome);
        return st.fc_target;
    case Applied::BadSha:
        snprintf(s_outcome, sizeof s_outcome,
                 "the staged full restore does not match its checksum: discarded, storage kept");
        break;
    case Applied::Parts:
        snprintf(s_outcome, sizeof s_outcome,
                 "the staged full restore does not fit this hub's partitions: discarded, storage kept");
        break;
    case Applied::Flash:
        // Some partitions may be half written: never run on them. The stage stays armed; the next boot starts over.
        ESP_LOGE(TAG, "writing the staged full restore failed (flash error): restarting to try again");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGE(TAG, "%s", s_outcome);
    return 0;
}

void backup_log_outcome() {
    if (s_outcome[0]) ESP_LOGW(TAG, "at boot: %s", s_outcome);
}

bool backup_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    u.method = HTTP_POST;
    u.uri = "/api/backup/full";
    u.handler = handle_full;
    bool ok = auth_register_uri(hd, &u) == ESP_OK;
    u.uri = "/api/backup/restore";
    u.handler = handle_restore;
    ok = auth_register_uri(hd, &u) == ESP_OK && ok;
    if (!ok) ESP_LOGE(TAG, "full backup routes not registered (no free web server handler slot?)");
    return ok;
}
