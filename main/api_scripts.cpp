// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_scripts.cpp — Lua script REST surface.
//
//   GET    /api/scripts                  — list {name, size}
//   GET    /api/scripts/<name>           — read source
//   PUT    /api/scripts/<name>           — write source (raw body, at most 16,384 bytes; more: 413)
//   DELETE /api/scripts/<name>           — delete
//   POST   /api/scripts/<name>/run       — enqueue invocation
//   POST   /api/scripts/<name>/check     — body = source; returns {ok,err,line}
//
// Names are alphanum + `_-`, ≤24 chars; enforced by lua_script_cache.
#include "auth.h"
#include "api_scripts.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ArduinoJson.h"
#include "lua_engine.h"
#include "lua_engine_scripts.h"
#include "ws_bridge.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "api_scripts";

// A script holds at most LUA_SCRIPT_SRC_MAX (16,384) bytes; buffers add one for the NUL, so a full-size
// script reads, checks and saves (spec 2026-10-05 §3.8). Heap, not the httpd task stack.
static constexpr size_t kScriptCap = LUA_SCRIPT_SRC_MAX;

static const char* uri_after_prefix(const char* uri, const char* prefix) {
    size_t pl = std::strlen(prefix);
    if (std::strncmp(uri, prefix, pl) != 0) return nullptr;
    return uri + pl;
}

// Pull name out of "/api/scripts/<name>" or "/api/scripts/<name>/run".
// Trims trailing /run or /check. Returns false on bad path.
static bool parse_name(const char* uri, char* out, size_t cap,
                       bool* is_run, bool* is_check) {
    const char* rest = uri_after_prefix(uri, "/api/scripts/");
    if (!rest || !*rest) return false;
    *is_run = *is_check = false;
    const char* end = std::strchr(rest, '/');
    size_t nlen;
    if (end) {
        nlen = (size_t)(end - rest);
        if (std::strcmp(end, "/run")   == 0) *is_run   = true;
        else if (std::strcmp(end, "/check") == 0) *is_check = true;
        else return false;
    } else {
        nlen = std::strlen(rest);
    }
    if (nlen == 0 || nlen >= cap) return false;
    std::memcpy(out, rest, nlen);
    out[nlen] = '\0';
    return true;
}

// The request body in a PSRAM buffer of kScriptCap + 1 bytes (NUL-terminated, *total bytes). A body over
// kScriptCap sets *too_large and returns nullptr: the hub used to keep the first 16,383 bytes of such a
// body and say ok. nullptr without *too_large: out of memory.
static char* read_body(httpd_req_t* req, int* total, bool* too_large) {
    *too_large = req->content_len > kScriptCap;
    if (*too_large) return nullptr;
    char* buf = static_cast<char*>(heap_caps_malloc(kScriptCap + 1, MALLOC_CAP_SPIRAM));
    if (!buf) return nullptr;
    int n = 0;
    while (n < static_cast<int>(req->content_len)) {
        const int got = httpd_req_recv(req, buf + n, req->content_len - n);
        if (got <= 0) break;
        n += got;
    }
    buf[n] = '\0';
    *total = n;
    return buf;
}

static esp_err_t too_large(httpd_req_t* req) {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "a script is at most 16384 bytes");
    return ESP_FAIL;
}

// ── GET /api/scripts ────────────────────────────────────────────────────
static esp_err_t handle_get_scripts(httpd_req_t* req) {
    LuaScriptEntry list[LUA_SCRIPT_MAX];
    uint16_t n = lua_script_cache_list(list, LUA_SCRIPT_MAX);

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint16_t i = 0; i < n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = list[i].name;
        o["size"] = list[i].size;
    }
    char buf[1024];
    size_t bn = serializeJson(doc, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, bn);
}

// ── Item handler covers GET/PUT/DELETE and POST .../run|/check ─────────
static esp_err_t handle_scripts_item(httpd_req_t* req) {
    char name[LUA_SCRIPT_NAME_MAX + 1];
    bool is_run = false, is_check = false;
    if (!parse_name(req->uri, name, sizeof(name), &is_run, &is_check)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        char* src = (char*)heap_caps_malloc(kScriptCap + 1, MALLOC_CAP_SPIRAM);
        if (!src) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
            return ESP_FAIL;
        }
        int n = lua_script_cache_read(name, src, kScriptCap + 1);
        if (n < 0) {
            heap_caps_free(src);
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "text/plain");
        esp_err_t e = httpd_resp_send(req, src, n);
        heap_caps_free(src);
        return e;
    }

    // Save. The SPA and net-core use POST /api/scripts/<name> (body = source);
    // this port only took PUT and answered a bare POST with 405 "unsupported".
    if (req->method == HTTP_PUT || (req->method == HTTP_POST && !is_run && !is_check)) {
        const bool existed = lua_script_cache_exists(name);
        int total = 0;
        bool over = false;
        char* src = read_body(req, &total, &over);
        if (over) return too_large(req);
        if (!src) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
            return ESP_FAIL;
        }
        const bool ok = lua_script_cache_write(name, src);
        // size and fp of what is stored: the store keeps the text up to its first NUL
        if (ok) ws_push_script_saved(name, src, std::strlen(src), existed);
        heap_caps_free(src);
        httpd_resp_set_type(req, "application/json");
        if (ok) return httpd_resp_sendstr(req, "{\"ok\":true}");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }

    if (req->method == HTTP_DELETE) {
        bool ok = lua_script_cache_delete(name);
        httpd_resp_set_type(req, "application/json");
        if (ok) {
            JsonDocument p; p["name"] = name;
            ws_push("script.deleted", p);
            return httpd_resp_sendstr(req, "{\"ok\":true}");
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    if (req->method == HTTP_POST && is_run) {
        bool ok = lua_engine_run_script(name);
        httpd_resp_set_type(req, "application/json");
        if (ok) return httpd_resp_sendstr(req, "{\"ok\":true,\"queued\":true}");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "queue saturated or missing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_POST && is_check) {
        int total = 0;
        bool over = false;
        char* src = read_body(req, &total, &over);
        if (over) return too_large(req);
        if (!src) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc"); return ESP_FAIL; }
        char err[128] = {0};
        int line = 0;
        bool ok = lua_engine_check_syntax(name, src, err, sizeof(err), &line);
        heap_caps_free(src);

        JsonDocument doc;
        doc["ok"]   = ok;
        doc["err"]  = err;
        doc["line"] = line;
        char buf[256];
        size_t bn = serializeJson(doc, buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf, bn);
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "unsupported");
    return ESP_FAIL;
}

bool api_scripts_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};

    u.uri = "/api/scripts"; u.method = HTTP_GET; u.handler = handle_get_scripts;
    auth_register_uri(hd, &u);

    u.uri = "/api/scripts/*"; u.handler = handle_scripts_item;
    u.method = HTTP_GET;    auth_register_uri(hd, &u);
    u.method = HTTP_PUT;    auth_register_uri(hd, &u);
    u.method = HTTP_DELETE; auth_register_uri(hd, &u);
    u.method = HTTP_POST;   auth_register_uri(hd, &u);

    ESP_LOGI(TAG, "/api/scripts CRUD + /run + /check registered");
    return true;
}
