// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_scripts.cpp — Lua script REST surface.
//
//   GET    /api/scripts                  — list {name, size}
//   GET    /api/scripts/<name>           — read source
//   PUT    /api/scripts/<name>           — write source (raw body)
//   DELETE /api/scripts/<name>           — delete
//   POST   /api/scripts/<name>/run       — enqueue invocation
//   POST   /api/scripts/<name>/check     — body = source; returns {ok,err,line}
//
// Names are alphanum + `_-`, ≤24 chars; enforced by lua_script_cache.
#include "api_scripts.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ArduinoJson.h"
#include "lua_engine.h"
#include "lua_engine_scripts.h"
#include "ws_bridge.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "api_scripts";

// Lua source soft cap from lua_engine_scripts.h is 16 KB. We allocate
// on the heap to avoid blowing the 8 KB httpd task stack.
static constexpr size_t kScriptCap = 16 * 1024;

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
        // Read source. Allocate on the heap — kScriptCap is too big for
        // the httpd task stack.
        char* src = (char*)heap_caps_malloc(kScriptCap, MALLOC_CAP_SPIRAM);
        if (!src) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
            return ESP_FAIL;
        }
        int n = lua_script_cache_read(name, src, kScriptCap);
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

    if (req->method == HTTP_PUT) {
        // Raw source body, up to kScriptCap-1 bytes.
        bool existed = lua_script_cache_exists(name);
        char* src = (char*)heap_caps_malloc(kScriptCap, MALLOC_CAP_SPIRAM);
        if (!src) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
            return ESP_FAIL;
        }
        int total = 0;
        while (total < (int)kScriptCap - 1) {
            int got = httpd_req_recv(req, src + total, kScriptCap - 1 - total);
            if (got <= 0) break;
            total += got;
        }
        src[total] = '\0';
        bool ok = lua_script_cache_write(name, src);
        heap_caps_free(src);
        httpd_resp_set_type(req, "application/json");
        if (ok) {
            JsonDocument p; p["name"] = name;
            ws_push(existed ? "script.updated" : "script.added", p);
            return httpd_resp_sendstr(req, "{\"ok\":true}");
        }
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
        char* src = (char*)heap_caps_malloc(kScriptCap, MALLOC_CAP_SPIRAM);
        if (!src) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc"); return ESP_FAIL; }
        int total = 0;
        while (total < (int)kScriptCap - 1) {
            int got = httpd_req_recv(req, src + total, kScriptCap - 1 - total);
            if (got <= 0) break;
            total += got;
        }
        src[total] = '\0';
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
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/scripts/*"; u.handler = handle_scripts_item;
    u.method = HTTP_GET;    httpd_register_uri_handler(hd, &u);
    u.method = HTTP_PUT;    httpd_register_uri_handler(hd, &u);
    u.method = HTTP_DELETE; httpd_register_uri_handler(hd, &u);
    u.method = HTTP_POST;   httpd_register_uri_handler(hd, &u);

    ESP_LOGI(TAG, "/api/scripts CRUD + /run + /check registered");
    return true;
}
