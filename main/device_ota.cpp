// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Zigbee device firmware update from the hub page (research 2026-11-18 C2).
//   POST /api/device/ota?ieee=0x...   the device's OTA file (application/octet-stream, at most 4 MB)
//   GET  /api/device/ota              the update's state
//   POST /api/device/ota/cancel       stop it; the device keeps its firmware
// One update at a time. The file is checked (device_ota_core.h), held in PSRAM and handed to the stack's OTA server
// (esp_zb_ota.cpp); the device is told to ask (Image Notify) and only it is served. Nothing is kept across a restart:
// the device's download then fails and it keeps the firmware it has.
#include "device_ota.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/socket.h>

#include "auth.h"
#include "device_ota_core.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "zigbee_mgr.h"

namespace {

const char* TAG = "device_ota";

// s_op: one upload, cancel or drop at a time (web server, main loop). s_mu: the fields below, also taken by the
// stack task inside its callbacks while it holds the stack's lock -- so nothing calls into the stack under s_mu.
SemaphoreHandle_t s_op;
SemaphoreHandle_t s_mu;
devota::Session s_s;
uint8_t* s_img = nullptr;   // the uploaded file (PSRAM); the stack serves s_img + s_s.h.start while it is loaded

struct Hold {
    SemaphoreHandle_t m;
    bool ok;
    Hold(SemaphoreHandle_t x, TickType_t wait = portMAX_DELAY) : m(x), ok(xSemaphoreTake(x, wait) == pdTRUE) {}
    ~Hold() { if (ok) xSemaphoreGive(m); }
};

uint32_t uptime_s() { return (uint32_t)(esp_timer_get_time() / 1000000); }

// ── The stack's side (stack task) ─────────────────────────────────────────

bool on_event(int kind, uint64_t ieee, uint16_t manuf, uint16_t type, uint32_t a) {
    Hold h(s_mu);
    switch (kind) {
    case ZB_OTA_START: {
        devota::Query q;
        q.manuf = manuf;
        q.image_type = type;
        q.version = a;
        const bool go = devota::on_start(s_s, ieee, q);
        if (go) ESP_LOGI(TAG, "0x%016" PRIX64 " runs v%08" PRIX32 ": sending v%08" PRIX32, ieee, a, s_s.h.file_version);
        else if (s_s.state == devota::State::Done && ieee == s_s.ieee) ESP_LOGI(TAG, "0x%016" PRIX64 " already runs v%08" PRIX32, ieee, a);
        return go;
    }
    case ZB_OTA_BLOCK:
        if (!devota::serves(s_s, ieee)) return false;
        if (devota::on_block(s_s, a)) ESP_LOGI(TAG, "0x%016" PRIX64 ": %d %%", ieee, devota::pct(s_s));
        return true;
    case ZB_OTA_END:
        if (!devota::serves(s_s, ieee)) return false;
        devota::on_end(s_s, (uint8_t)a);
        if (a == 0) ESP_LOGI(TAG, "0x%016" PRIX64 " has the whole image and applies it", ieee);
        else ESP_LOGW(TAG, "0x%016" PRIX64 " refused the image (status 0x%02" PRIX32 ")", ieee, a);
        return true;
    case ZB_OTA_ABORT:
        if (devota::serves(s_s, ieee)) {
            devota::fail(s_s, "device_aborted");
            ESP_LOGW(TAG, "0x%016" PRIX64 ": the download was given up", ieee);
        }
        return true;
    default:
        return true;
    }
}

void on_frame(uint64_t ieee, const uint8_t* zcl, size_t n) {
    devota::Query q;
    if (!devota::parse_query(zcl, n, &q)) return;
    Hold h(s_mu);
    devota::on_query(s_s, ieee, q);
}

// ── The hub's side (web server, main loop) ────────────────────────────────

// With s_op held: an ended update's image out of the stack and freed. False while it is still loaded (the stack's
// lock timed out; the next tick tries again).
bool drop_ended() {
    {
        Hold h(s_mu);
        if (!s_img) return true;
        if (devota::active(s_s)) return false;
    }
    if (!esp_zb_ota_unload()) return false;
    Hold h(s_mu);
    heap_caps_free(s_img);
    s_img = nullptr;
    return true;
}

esp_err_t reply(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

// ws_server fails a request's reads 8 s after its first byte until the hub answers (backup.cpp has the same need):
// the upload reads through its own function, with one limit for the whole file.
constexpr int64_t kUploadBudgetUs = 90 * 1000 * 1000;
int64_t s_upload_until;

int upload_recv(httpd_handle_t, int fd, char* buf, size_t len, int flags) {
    if (!buf) return HTTPD_SOCK_ERR_INVALID;
    if (esp_timer_get_time() > s_upload_until) return HTTPD_SOCK_ERR_FAIL;
    const int r = recv(fd, buf, len, flags);
    if (r >= 0) return r;
    return errno == EAGAIN || errno == EINTR ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
}

bool recv_all(httpd_req_t* req, uint8_t* p, size_t n) {
    int timeouts = 0;
    while (n) {
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(p), n);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (r <= 0) return false;
        timeouts = 0;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

// Everything up to the stack: the answer, or nullptr with `img` taken by the session.
const char* upload(httpd_req_t* req, const char** status, uint64_t* ieee_out) {
    char q[64] = {}, v[24] = {}, ct[48] = {};
    *status = "400 Bad Request";
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "ieee", v, sizeof v) != ESP_OK)
        return "{\"error\":\"no_device\"}";
    const uint64_t ieee = strtoull(v, nullptr, 16);
    // A page of another site cannot send this type without a CORS preflight the hub never grants.
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) != ESP_OK ||
        strncasecmp(ct, "application/octet-stream", 24) != 0)
        return "{\"error\":\"content_type\"}";
    if (req->content_len == 0) return "{\"error\":\"not_an_ota_file\"}";
    if (req->content_len > devota::kMaxFile) {
        *status = "413 Payload Too Large";
        return "{\"error\":\"too_large\"}";
    }
    ZapDevice dev{};
    if (!ieee || !zigbee_pool_snapshot(ieee, &dev) || zap_dev_is_removed(&dev) || !dev.nwk_addr) {
        *status = "404 Not Found";
        return "{\"error\":\"unknown_device\"}";
    }
    *status = "409 Conflict";
    if (!esp_zigbee_backend_network_up()) return "{\"error\":\"radio_down\"}";
    {
        Hold h(s_mu);
        if (devota::active(s_s)) return "{\"error\":\"busy\"}";
    }
    if (!drop_ended()) {
        *status = "503 Service Unavailable";
        return "{\"error\":\"radio_busy\"}";
    }
    uint8_t* img = static_cast<uint8_t*>(heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!img) {
        *status = "503 Service Unavailable";
        return "{\"error\":\"no_memory\"}";
    }
    const int fd = httpd_req_to_sockfd(req);
    s_upload_until = esp_timer_get_time() + kUploadBudgetUs;
    httpd_sess_set_recv_override(req->handle, fd, upload_recv);
    devota::Header hd;
    devota::FileErr fe = devota::FileErr::NotOta;
    const bool got = recv_all(req, img, req->content_len);
    if (got) fe = devota::parse(img, req->content_len, &hd);
    if (!got || fe != devota::FileErr::None || !esp_zb_ota_load(img + hd.start, hd.total)) {
        heap_caps_free(img);
        *status = "400 Bad Request";
        if (!got) return "{\"error\":\"incomplete\"}";
        if (fe != devota::FileErr::None) return "{\"error\":\"not_an_ota_file\"}";
        *status = "409 Conflict";
        return "{\"error\":\"radio_down\"}";
    }
    {
        Hold h(s_mu);
        devota::begin(s_s, ieee, hd, uptime_s());   // s_op held and checked inactive above: cannot be refused
        s_img = img;
    }
    const bool told = esp_zb_ota_notify(dev.nwk_addr);
    ESP_LOGI(TAG, "update of 0x%016" PRIX64 ": image %04X/%04X v%08" PRIX32 ", %" PRIu32 " bytes; %s", ieee, hd.manuf,
             hd.image_type, hd.file_version, hd.total, told ? "device told" : "waits for the device's own query");
    *ieee_out = ieee;
    return nullptr;
}

esp_err_t handle_upload(httpd_req_t* req) {
    Hold op(s_op);
    const char* status = nullptr;
    uint64_t ieee = 0;
    const int fd = httpd_req_to_sockfd(req);
    const char* err = upload(req, &status, &ieee);
    // Once its reads went through upload_recv the session has left ws_server's guard: it closes after this answer
    // (httpd reads the rest of an unread body first, within the upload's budget).
    const bool unguarded = s_upload_until != 0;
    s_upload_until = 0;
    esp_err_t r;
    if (err) {
        r = reply(req, status, err);
    } else {
        char out[128];
        Hold h(s_mu);
        snprintf(out, sizeof out, "{\"manuf\":%u,\"image_type\":%u,\"file_version\":%" PRIu32 ",\"size\":%" PRIu32 "}",
                 s_s.h.manuf, s_s.h.image_type, s_s.h.file_version, s_s.h.total);
        r = reply(req, "200 OK", out);
    }
    if (unguarded) httpd_sess_trigger_close(req->handle, fd);
    return r;
}

esp_err_t send_state(httpd_req_t* req) {
    char out[448];
    {
        Hold h(s_mu);
        const devota::Session& s = s_s;
        if (s.state == devota::State::Idle) return reply(req, "200 OK", "{\"state\":\"idle\"}");
        int n = snprintf(out, sizeof out,
                         "{\"state\":\"%s\",\"ieee\":\"0x%016" PRIX64 "\",\"manuf\":%u,\"image_type\":%u,"
                         "\"file_version\":%" PRIu32 ",\"size\":%" PRIu32 ",\"offset\":%" PRIu32 ",\"pct\":%d,"
                         "\"fits\":%s,\"reason\":\"%s\",\"device_status\":%u,\"elapsed_s\":%" PRIu32,
                         devota::state_name(s.state), s.ieee, s.h.manuf, s.h.image_type, s.h.file_version, s.h.total,
                         s.offset, devota::pct(s), devota::fits(s) ? "true" : "false", s.reason, s.device_status,
                         uptime_s() - s.since);
        if (s.has_query)
            n += snprintf(out + n, sizeof out - n,
                          ",\"device_manuf\":%u,\"device_image_type\":%u,\"device_version\":%" PRIu32, s.dev.manuf,
                          s.dev.image_type, s.dev.version);
        snprintf(out + n, sizeof out - n, "}");
    }
    return reply(req, "200 OK", out);
}

esp_err_t handle_state(httpd_req_t* req) { return send_state(req); }

esp_err_t handle_cancel(httpd_req_t* req) {
    Hold op(s_op);
    uint64_t was = 0;
    {
        Hold h(s_mu);
        if (devota::active(s_s)) was = s_s.ieee;
        devota::cancel(s_s);
    }
    if (was) ESP_LOGI(TAG, "update of 0x%016" PRIX64 " cancelled", was);
    drop_ended();
    return send_state(req);
}

}  // namespace

void device_ota_tick() {
    if (!s_op) return;
    Hold op(s_op, 0);   // an upload holds it for seconds: the next tick then
    if (!op.ok) return;
    uint64_t ieee = 0;
    {
        Hold h(s_mu);
        if (devota::active(s_s)) ieee = s_s.ieee;
    }
    ZapDevice dev{};
    if (ieee && (!zigbee_pool_snapshot(ieee, &dev) || zap_dev_is_removed(&dev))) {
        Hold h(s_mu);
        devota::fail(s_s, "device_left");
        ESP_LOGW(TAG, "0x%016" PRIX64 " left the hub: update ended", ieee);
    }
    drop_ended();
}

bool device_ota_register(httpd_handle_t hd) {
    if (!hd) return false;
    if (!s_mu) {
        s_op = xSemaphoreCreateMutex();
        s_mu = xSemaphoreCreateMutex();
        if (!s_op || !s_mu) return false;
        esp_zb_ota_handlers(on_event, on_frame);
    }
    httpd_uri_t u{};
    u.uri = "/api/device/ota";
    u.method = HTTP_POST;
    u.handler = handle_upload;
    bool ok = auth_register_uri(hd, &u) == ESP_OK;
    u.method = HTTP_GET;
    u.handler = handle_state;
    ok = auth_register_uri(hd, &u) == ESP_OK && ok;
    u.uri = "/api/device/ota/cancel";
    u.method = HTTP_POST;
    u.handler = handle_cancel;
    ok = auth_register_uri(hd, &u) == ESP_OK && ok;
    if (!ok) ESP_LOGE(TAG, "device firmware update routes not registered (no free web server handler slot?)");
    return ok;
}
