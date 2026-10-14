// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Turning sign-in on must sign out every WebSocket that was let in while it was off (review F02):
// those sockets never showed a token, and the dispatcher trusts the per-socket flag.
// Wrong WebSocket tokens lock out the peer that sent them, not every new login (review F31).
#include "auth.h"

#include <cstring>
#include <map>
#include <string>

// ── fakes ───────────────────────────────────────────────────────────────────
static std::map<std::string, std::string> g_nvs;   // "key" -> bytes (one namespace is enough here)
static std::string g_ws_token;                     // what ws_server would compare against
static bool        g_ws_socket_authed = false;     // one open browser socket

int64_t esp_timer_get_time() { return 1000000; }
void esp_fill_random(void* buf, size_t len) { memset(buf, 0x5A, len); }
SemaphoreHandle_t xSemaphoreCreateMutex() { static int m; return &m; }
int xSemaphoreTake(SemaphoreHandle_t, int) { return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }

esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* out) { *out = 1; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t, const char* k, uint8_t* out) {
    auto it = g_nvs.find(k); if (it == g_nvs.end()) return ESP_FAIL;
    *out = static_cast<uint8_t>(it->second[0]); return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t, const char* k, uint8_t v) { g_nvs[k] = std::string(1, char(v)); return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t, const char* k, char* out, size_t* len) {
    auto it = g_nvs.find(k); if (it == g_nvs.end() || it->second.size() + 1 > *len) return ESP_FAIL;
    memcpy(out, it->second.c_str(), it->second.size() + 1); return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char* k, const char* v) { g_nvs[k] = v; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { return ESP_FAIL; }
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void nvs_close(nvs_handle_t) {}

psa_status_t psa_crypto_init() { return PSA_SUCCESS; }
psa_status_t psa_hash_compute(int, const uint8_t*, size_t, uint8_t*, size_t, size_t*) { return -1; }

int httpd_req_to_sockfd(httpd_req_t*) { return -1; }
// Socket fd -> peer IPv4, as getpeername() would report it (linked with --wrap=getpeername).
extern "C" int __wrap_getpeername(int fd, struct sockaddr* sa, socklen_t* len) {
    auto* in = reinterpret_cast<struct sockaddr_in*>(sa);
    std::memset(sa, 0, *len);
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = 0x0A000000u + static_cast<uint32_t>(fd);   // 10.0.0.<fd>
    return 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, size_t) { return ESP_FAIL; }
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_req_recv(httpd_req_t*, char*, size_t) { return 0; }
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*) { return ESP_OK; }

void ws_server_set_api_token(const char* t) { g_ws_token = t ? t : ""; }
void ws_server_fd_deauth_all() { g_ws_socket_authed = false; }
uint32_t zap_setup_secs_left() { return 0; }

// ── checks ──────────────────────────────────────────────────────────────────
static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    auth_init();                                  // nothing stored, default off
    expect("sign-in starts off", !auth_enabled());
    expect("no token installed while off", g_ws_token.empty());

    g_ws_socket_authed = true;                    // a browser that connected while sign-in was off
    auth_set_enabled(true);
    expect("turning sign-in on installs the token", g_ws_token.size() == 32);
    expect("turning sign-in on signs that socket out", !g_ws_socket_authed);
    expect("a wrong token is still refused", !auth_check_token("00000000000000000000000000000000", 5));

    g_ws_socket_authed = true;                    // it signed in with the token
    auth_set_enabled(true);                       // saving "on" again is no transition
    expect("saving on again keeps signed-in sockets", g_ws_socket_authed);

    // One peer guessing must not lock out another peer's valid login.
    const std::string good = g_ws_token;
    for (int i = 0; i < 6; i++) auth_check_token("00000000000000000000000000000000", 7);
    expect("the guessing peer is locked out", !auth_check_token(good.c_str(), 7));
    expect("another peer still signs in", auth_check_token(good.c_str(), 8));

    auth_set_enabled(false);
    expect("turning sign-in off removes the token", g_ws_token.empty());

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nauth OK\n", g_fail);
    return g_fail;
}
