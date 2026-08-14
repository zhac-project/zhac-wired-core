// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// spa_serve.cpp — mount the `spa` SPIFFS partition and serve files
// from it with a wildcard handler.
//
// URL → file mapping:
//   /                → /spa/index.html
//   /assets/foo.js   → /spa/assets/foo.js
//   anything else    → /spa/index.html (SPA history-router fallback)
//
// The SPA mount happens regardless of whether the partition contains
// a built dist/ tree; an empty partition just yields 404s for asset
// requests and the existing placeholder root handler keeps serving
// "alive" text. Upload the SPA via `idf.py spiffs-gen` or `parttool`
// + `spiffsgen.py` separately.

#include "spa_serve.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

static const char* TAG = "spa";

static bool s_mounted = false;

static const char* mime_for(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".woff2")) return "font/woff2";
    if (!strcmp(dot, ".map"))  return "application/json";
    return "application/octet-stream";
}

static esp_err_t stream_file(httpd_req_t* req, const char* full_path) {
    FILE* f = fopen(full_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    httpd_resp_set_type(req, mime_for(full_path));
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f); return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// Catchall — `/*` matches every URL the more specific handlers didn't
// claim. Translate URL to /spa/<rest>; if that's a real file, stream
// it. Otherwise serve index.html (history-router fallback).
static esp_err_t handle_spa_catchall(httpd_req_t* req) {
    if (!s_mounted) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "spa partition not mounted");
        return ESP_FAIL;
    }
    char full[160];
    const char* uri = req->uri;
    if (uri[0] == '/' && uri[1] == '\0') uri = "/index.html";
    int n = snprintf(full, sizeof(full), "/spa%s", uri);
    if (n >= (int)sizeof(full)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long");
        return ESP_FAIL;
    }
    struct stat st{};
    if (stat(full, &st) == 0) return stream_file(req, full);

    // History-router fallback: serve index.html for any unknown path.
    return stream_file(req, "/spa/index.html");
}

bool spa_mount() {
    esp_vfs_spiffs_conf_t cfg{};
    cfg.base_path              = "/spa";
    cfg.partition_label        = "spa";
    cfg.max_files              = 6;
    cfg.format_if_mount_failed = false;
    esp_err_t e = esp_vfs_spiffs_register(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "spa partition mount failed: %s — REST still works",
                 esp_err_to_name(e));
        s_mounted = false;
        return false;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("spa", &total, &used);
    ESP_LOGI(TAG, "spa mounted at /spa  (%u/%u bytes used)",
             (unsigned)used, (unsigned)total);
    s_mounted = true;
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
