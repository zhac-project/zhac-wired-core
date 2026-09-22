// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth.h"
#include "api_remote.h"
#include "sdkconfig.h"

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE

#include <cstdio>
#include <cstring>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "remote_client.h"
#include "remote_nvs.h"
#include "remote_state.h"

static const char* TAG = "api_remote";

size_t remote_status_json(char* out, size_t cap) {
    RemoteStatusSnap s{};
    remote_client_get_status(&s);
    // State by name, as net-core sends it: the web UI's badge maps "READY" to
    // "Connected" and showed nothing for a bare number.
    return (size_t)snprintf(out, cap,
        "{\"enabled\":%s,\"state\":\"%s\",\"connected_since\":%u,\"last_event_at\":%u,"
        "\"rtt_ms\":%u,\"tx_drops\":%u,\"auth_fails\":%u}",
        s.enabled ? "true" : "false", remote_state_name((RemoteState)s.state),
        (unsigned)s.connected_since, (unsigned)s.last_event_at,
        (unsigned)s.rtt_ms, (unsigned)s.tx_drops, (unsigned)s.auth_fails);
}

const char* remote_connect_req(const char* body, size_t len) {
    JsonDocument d;
    if (!body || len == 0 || deserializeJson(d, body, len)) return "bad request";
    const char* url = d["url"]       | (const char*)"";
    const char* tok = d["token"]     | (const char*)"";
    const char* did = d["device_id"] | (const char*)"";   // "" keeps stored
    if (!url[0] || !tok[0]) return "url + token required";
    // DS9, as on net-core: TLS only. A ws:// link would carry the bearer token and
    // the whole device-control surface in cleartext, and the server-certificate
    // check (esp_crt_bundle) only applies to wss://.
    if (std::strncmp(url, "wss://", 6) != 0) return "url must start with wss://";
    // remote_nvs_load() reads into fixed buffers and fails on anything longer, which
    // left an over-long value saved and the link dead at every boot. Refuse it here.
    if (std::strlen(url) >= REMOTE_NVS_URL_MAX)   return "url too long";
    if (std::strlen(tok) >= REMOTE_NVS_TOKEN_MAX) return "token too long";
    if (std::strlen(did) >= REMOTE_NVS_DEVID_MAX) return "device_id too long";
    if (!remote_nvs_save(true, url, tok, did)) return "could not save";
    remote_client_enable();
    return nullptr;
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
    char body[512];   // room for an over-long value to reach the length checks
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[n] = '\0';
    httpd_resp_set_type(req, "application/json");
    if (const char* err = remote_connect_req(body, (size_t)n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
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
    u.uri = "/api/remote/status";     u.method = HTTP_GET;  u.handler = h_status;     auth_register_uri(hd, &u);
    u.uri = "/api/remote/connect";    u.method = HTTP_POST; u.handler = h_connect;    auth_register_uri(hd, &u);
    u.uri = "/api/remote/disconnect"; u.method = HTTP_POST; u.handler = h_disconnect; auth_register_uri(hd, &u);
    ESP_LOGI(TAG, "remote routes registered");
    return true;
}

#else  // CONFIG_ZHAC_REMOTE_CLIENT_ENABLE off — link-only stubs

size_t remote_status_json(char*, size_t) { return 0; }
const char* remote_connect_req(const char*, size_t) { return "cloud link not built"; }
bool   remote_disconnect_req(const char*, size_t, bool* f) { if (f) *f = false; return false; }
bool   api_remote_register(httpd_handle_t) { return true; }

#endif
