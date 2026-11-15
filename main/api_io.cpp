// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// GET/PUT /api/io: the hub page's pin editor (spec §6). Local only: the cloud
// link has no route here, pin wiring is decided at the hub.
#include "api_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "auth.h"
#include "edit_lock.h"
#include "esp_log.h"
#include "http_body.h"
#include "sdkconfig.h"
#include "user_io.h"

namespace {

constexpr const char* TAG = "api_io";
constexpr size_t kBodyMax = 4096;   // 16 full rows are ~2.5 KB
constexpr size_t kOutMax  = 6144;

// The GET body, also the PUT answer. Bytes written, 0 if it did not fit.
size_t state_json(char* out, size_t cap) {
    auto* st = static_cast<UserIoState*>(std::calloc(1, sizeof(UserIoState)));
    if (!st) return 0;
    user_io_snapshot(st);
    const UserIoCaps& caps = user_io_caps();
    size_t n = 0;
    bool ok = true;
    auto put = [&](const char* fmt, auto... args) {
        if (!ok) return;
        const int m = std::snprintf(out + n, cap - n, fmt, args...);
        if (m < 0 || static_cast<size_t>(m) >= cap - n) { ok = false; return; }
        n += static_cast<size_t>(m);
    };
    put("{\"target\":\"%s\",\"adc_unit\":\"%s\",\"max\":%u,\"allowed\":[", CONFIG_IDF_TARGET,
        user_io_adc_in_mv() ? "mV" : "raw", static_cast<unsigned>(kUserIoMaxPins));
    for (uint8_t i = 0; i < caps.n_allowed; i++) put("%s%u", i ? "," : "", caps.allowed[i]);
    put("],\"adc_pins\":[");
    bool first = true;
    for (uint8_t i = 0; i < caps.n_allowed; i++) {
        if (!(caps.adc_mask & (1ULL << caps.allowed[i]))) continue;
        put("%s%u", first ? "" : ",", caps.allowed[i]);
        first = false;
    }
    put("],\"pins\":[");
    for (uint8_t i = 0; i < st->cfg.count && ok; i++) {
        char row[256];
        const size_t m = user_io_cfg_pin_json(st->cfg.pins[i], row, sizeof(row));
        if (!m) { ok = false; break; }
        row[m - 1] = '\0';   // reopen the object for the runtime fields
        // err texts are fixed firmware strings: no quotes or backslashes to escape.
        put("%s%s,\"value\":%ld,\"err\":\"%s\"}", i ? "," : "", row, static_cast<long>(st->value[i]), st->err[i]);
    }
    put("]}");
    std::free(st);
    return ok ? n : 0;
}

esp_err_t reply_state(httpd_req_t* req) {
    char* out = static_cast<char*>(std::malloc(kOutMax));
    const size_t n = out ? state_json(out, kOutMax) : 0;
    if (!n) {
        std::free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t e = httpd_resp_send(req, out, static_cast<ssize_t>(n));
    std::free(out);
    return e;
}

esp_err_t reply_error(httpd_req_t* req, const char* err) {
    // Validation texts quote only pin names and numbers ([a-z0-9_]): safe in a JSON string.
    char out[192];
    std::snprintf(out, sizeof(out), "{\"error\":\"%s\"}", err);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_FAIL;
}

esp_err_t handle_get(httpd_req_t* req) { return reply_state(req); }

esp_err_t handle_put(httpd_req_t* req) {
    char* body = static_cast<char*>(std::malloc(kBodyMax));
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    const int n = http_read_body(req, body, kBodyMax);
    if (n <= 0) { std::free(body); return reply_error(req, "body must be {\\\"pins\\\":[...]} under 4 KB"); }
    auto* cfg = static_cast<UserIoConfig*>(std::calloc(1, sizeof(UserIoConfig)));
    char err[96] = "";
    bool ok = cfg && user_io_cfg_from_json(body, static_cast<size_t>(n), user_io_caps(), cfg, err, sizeof(err));
    std::free(body);
    if (!cfg) std::snprintf(err, sizeof(err), "out of memory");
    if (ok) {
        EditLock lock(g_edit_mutex);   // one config change at a time, and not inside a rule edit
        ok = user_io_apply(*cfg, err, sizeof(err));
    }
    std::free(cfg);
    if (!ok) {
        // The parser's messages put quotes around nothing but the body hint; escape those.
        char esc[160];
        size_t k = 0;
        for (const char* p = err; *p && k + 2 < sizeof(esc); p++) {
            if (*p == '"' || *p == '\\') esc[k++] = '\\';
            esc[k++] = *p;
        }
        esc[k] = '\0';
        ESP_LOGW(TAG, "PUT refused: %s", err);
        return reply_error(req, esc);
    }
    return reply_state(req);
}

}  // namespace

bool api_io_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    u.uri = "/api/io"; u.method = HTTP_GET; u.handler = handle_get;
    auth_register_uri(hd, &u);
    u.method = HTTP_PUT; u.handler = handle_put;
    auth_register_uri(hd, &u);
    return true;
}
