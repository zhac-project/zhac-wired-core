// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "api_remote.h"
#include "sdkconfig.h"

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE

#include <cstdio>
#include <cstring>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "remote_client.h"
#include "remote_nvs.h"

static const char* TAG = "api_remote";

size_t remote_status_json(char* out, size_t cap) {
    RemoteStatusSnap s{};
    remote_client_get_status(&s);
    return (size_t)snprintf(out, cap,
        "{\"enabled\":%s,\"state\":%u,\"connected_since\":%u,\"last_event_at\":%u,"
        "\"rtt_ms\":%u,\"tx_drops\":%u,\"auth_fails\":%u}",
        s.enabled ? "true" : "false", (unsigned)s.state,
        (unsigned)s.connected_since, (unsigned)s.last_event_at,
        (unsigned)s.rtt_ms, (unsigned)s.tx_drops, (unsigned)s.auth_fails);
}

bool remote_connect_req(const char* body, size_t len) {
    JsonDocument d;
    if (!body || len == 0 || deserializeJson(d, body, len)) return false;
    const char* url = d["url"]       | (const char*)"";
    const char* tok = d["token"]     | (const char*)"";
    const char* did = d["device_id"] | (const char*)"";   // "" keeps stored
    if (!url[0] || !tok[0]) return false;
    if (!remote_nvs_save(true, url, tok, did)) return false;
    remote_client_enable();
    return true;
}

bool remote_disconnect_req(const char* body, size_t len, bool* forget_out) {
    bool forget = false;
    if (body && len) {
        JsonDocument d;
        if (!deserializeJson(d, body, len)) forget = d["forget"] | false;
    }
    remote_client_disable(forget);
    if (forget_out) *forget_out = forget;
    return true;
}

static esp_err_t h_status(httpd_req_t* req) {
    char b[256];
    size_t n = remote_status_json(b, sizeof(b));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, n);
}

static esp_err_t h_connect(httpd_req_t* req) {
    char body[320];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[n] = '\0';
    httpd_resp_set_type(req, "application/json");
    if (!remote_connect_req(body, (size_t)n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url + token required");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_disconnect(httpd_req_t* req) {
    char body[64];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) n = 0;
    body[n] = '\0';
    bool forget = false;
    remote_disconnect_req(body, (size_t)n, &forget);
    httpd_resp_set_type(req, "application/json");
    char r[48];
    int rl = snprintf(r, sizeof(r), "{\"ok\":true,\"forget\":%s}", forget ? "true" : "false");
    return httpd_resp_send(req, r, rl);
}

bool api_remote_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    u.uri = "/api/remote/status";     u.method = HTTP_GET;  u.handler = h_status;     httpd_register_uri_handler(hd, &u);
    u.uri = "/api/remote/connect";    u.method = HTTP_POST; u.handler = h_connect;    httpd_register_uri_handler(hd, &u);
    u.uri = "/api/remote/disconnect"; u.method = HTTP_POST; u.handler = h_disconnect; httpd_register_uri_handler(hd, &u);
    ESP_LOGI(TAG, "remote routes registered");
    return true;
}

#else  // CONFIG_ZHAC_REMOTE_CLIENT_ENABLE off — link-only stubs

size_t remote_status_json(char*, size_t) { return 0; }
bool   remote_connect_req(const char*, size_t) { return false; }
bool   remote_disconnect_req(const char*, size_t, bool* f) { if (f) *f = false; return false; }
bool   api_remote_register(httpd_handle_t) { return true; }

#endif
