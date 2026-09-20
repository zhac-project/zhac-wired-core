// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// spa_serve.cpp — serve the web UI from the pack embedded in the app image.
//
// tools/pack_spa.py gzips www-spa's dist/ into one blob at build time and
// main/CMakeLists.txt links it in (target_add_binary_data). Because the UI
// ships inside the app, an OTA update replaces firmware and UI together —
// the SPIFFS `spa` partition this replaced could not be updated over the air.
//
// URL -> file:
//   /                -> /index.html
//   /assets/foo.js   -> /assets/foo.js
//   anything else    -> /index.html (hash-routed SPA fallback)
// Bodies are sent gzip-encoded as packed. Hashed /assets/* are cached for a
// year; index.html is revalidated every time, so an update shows at once.
#include "spa_serve.h"

#include <cstdint>
#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"

extern const uint8_t spa_pack_start[] asm("_binary_spa_pack_start");
extern const uint8_t spa_pack_end[]   asm("_binary_spa_pack_end");

static const char* TAG = "spa";
static uint32_t s_count = 0;

// The CSP the web UI's README asks the firmware to send (a <meta> CSP cannot
// cover the WebSocket). 'unsafe-inline' styles: CodeMirror injects them.
static constexpr char kCsp[] =
    "default-src 'self'; connect-src 'self' ws: wss:; script-src 'self'; "
    "style-src 'self' 'unsafe-inline'; img-src 'self' data:; base-uri 'none'; "
    "frame-ancestors 'none'";

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Walk the pack's index for `path`. A handful of files: no table needed.
static const uint8_t* s_data = nullptr;   // start of the gzip bodies

static bool find(const char* path, const uint8_t** body, uint32_t* size) {
    const uint8_t* p = spa_pack_start + 12;
    const size_t plen = strlen(path);
    for (uint32_t i = 0; i < s_count; i++) {
        const uint16_t n = rd16(p);
        const uint8_t* name = p + 2;
        if (n == plen && memcmp(name, path, n) == 0) {
            const uint32_t off = rd32(name + n), len = rd32(name + n + 4);
            if (s_data + off + len > spa_pack_end) return false;
            *body = s_data + off;
            *size = len;
            return true;
        }
        p += 2 + n + 8;
    }
    return false;
}

static const char* mime_for(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

static esp_err_t handle_spa_catchall(httpd_req_t* req) {
    if (s_count == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req,
            "ZHAC is running, but this firmware was built without the web UI "
            "(www-spa dist/ was missing). The REST and WebSocket APIs work.\n");
    }
    char path[128];
    const char* uri = req->uri;
    const char* q = strchr(uri, '?');
    size_t n = q ? static_cast<size_t>(q - uri) : strlen(uri);
    if (n >= sizeof(path)) n = sizeof(path) - 1;
    memcpy(path, uri, n);
    path[n] = '\0';
    if (strcmp(path, "/") == 0) strcpy(path, "/index.html");

    const uint8_t* body = nullptr;
    uint32_t size = 0;
    if (!find(path, &body, &size)) {
        strcpy(path, "/index.html");
        if (!find(path, &body, &size)) return httpd_resp_send_404(req);
    }
    const bool is_index = strcmp(path, "/index.html") == 0;
    httpd_resp_set_type(req, mime_for(path));
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control",
                       is_index ? "no-cache" : "public, max-age=31536000, immutable");
    if (is_index) httpd_resp_set_hdr(req, "Content-Security-Policy", kCsp);
    return httpd_resp_send(req, reinterpret_cast<const char*>(body), size);
}

bool spa_mount() {
    const size_t len = static_cast<size_t>(spa_pack_end - spa_pack_start);
    if (len < 12 || memcmp(spa_pack_start, "ZSPA", 4) != 0 || rd32(spa_pack_start + 4) != 1) {
        ESP_LOGE(TAG, "embedded web UI pack is corrupt (%u B)", (unsigned)len);
        return false;
    }
    s_count = rd32(spa_pack_start + 8);
    const uint8_t* p = spa_pack_start + 12;             // index -> data offset
    for (uint32_t i = 0; i < s_count; i++) {
        if (p + 2 > spa_pack_end) { s_count = 0; break; }
        p += 2 + rd16(p) + 8;
    }
    s_data = p;
    if (s_count == 0) {
        ESP_LOGW(TAG, "built without the web UI (www-spa dist/ missing) -- REST/WS only");
        return false;
    }
    ESP_LOGI(TAG, "web UI: %u files, %u B embedded", (unsigned)s_count, (unsigned)len);
    return true;
}

bool spa_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    u.uri     = "/*";
    u.method  = HTTP_GET;
    u.handler = handle_spa_catchall;
    httpd_register_uri_handler(hd, &u);
    ESP_LOGI(TAG, "SPA catchall GET /* registered");
    return true;
}
