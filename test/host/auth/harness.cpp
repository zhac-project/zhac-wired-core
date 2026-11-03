// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Turning sign-in on must sign out every WebSocket that was let in while it was off (review F02):
// those sockets never showed a token, and the dispatcher trusts the per-socket flag.
// Wrong WebSocket tokens lock out the peer that sent them, not every new login (review F31).
// Review 2026-10-24: sign-in bodies must be sent as JSON (HS-05); a locked-out peer cannot be freed by
// cycling source addresses, IPv6 peers count per /64, wrong passwords share one hub-wide budget, and a
// lockout answers 429 (HS-06); the serial console shows the full token only while no password is set
// or after a password lockout asked for it, and the first claim gets a fresh token (HS-07).
#include "auth.h"

#include <arpa/inet.h>
#include <cstdarg>
#include <cstring>
#include <map>
#include <string>

// ── fakes ───────────────────────────────────────────────────────────────────
static std::map<std::string, std::string> g_nvs;   // "key" -> bytes (one namespace is enough here)
static std::string g_ws_token;                     // what ws_server would compare against
static bool        g_ws_socket_authed = false;     // one open browser socket
static int64_t     g_now_us = 1000000;
static uint32_t    g_setup_left = 0;               // the first-claim window
static std::string g_serial;                       // what auth.cpp printed on the console

int64_t esp_timer_get_time() { return g_now_us; }
static bool g_entropy_on = false;      // between bootloader_random_enable and _disable
static int  g_entropy_calls = 0;
static bool g_last_fill_entropy = false;
void bootloader_random_enable(void)  { g_entropy_on = true;  g_entropy_calls++; }
void bootloader_random_disable(void) { g_entropy_on = false; g_entropy_calls++; }
void esp_fill_random(void* buf, size_t len) {
    static uint8_t n = 0;
    for (size_t i = 0; i < len; i++) static_cast<uint8_t*>(buf)[i] = n++;
    g_last_fill_entropy = g_entropy_on;
}
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
esp_err_t nvs_get_blob(nvs_handle_t, const char* k, void* out, size_t* len) {
    auto it = g_nvs.find(k); if (it == g_nvs.end() || it->second.size() > *len) return ESP_FAIL;
    memcpy(out, it->second.data(), it->second.size()); *len = it->second.size(); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char* k, const void* v, size_t len) {
    g_nvs[k] = std::string(static_cast<const char*>(v), len); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char* k) { return g_nvs.erase(k) ? ESP_OK : ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void nvs_close(nvs_handle_t) {}

psa_status_t psa_crypto_init() { return PSA_SUCCESS; }
// FNV-1a spread over 32 bytes: a stand-in that tells passwords apart, not SHA-256.
psa_status_t psa_hash_compute(int, const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* olen) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= in[i]; h *= 1099511628211ull; }
    for (size_t i = 0; i < 32 && i < cap; i++) { h ^= i; h *= 1099511628211ull; out[i] = uint8_t(h >> 56); }
    *olen = 32;
    return PSA_SUCCESS;
}

// One request at a time: its headers and body, and what the handler answered.
static std::map<std::string, std::string> g_hdr;
static std::map<std::string, std::string> g_resp_hdr;   // what the handler set, by name
static int g_resp_hdr_dups = 0;                         // a header set twice on one response
static std::string g_body, g_status, g_resp, g_retry_after;
static std::map<std::string, httpd_uri_t> g_routes;

int httpd_req_to_sockfd(httpd_req_t* r) { return r->fd; }
// Socket fd -> peer, as getpeername() would report it (linked with --wrap=getpeername):
//   fd < 1000          IPv4 10.0.0.<fd>
//   1000 <= fd < 2000  IPv6 2001:db8:0:<(fd-1000)/10>::<fd%10 + 1>, so ten fds share each /64
//   2000 <= fd         IPv4-mapped ::ffff:10.0.0.<fd-2000> on a dual-stack socket
extern "C" int __wrap_getpeername(int fd, struct sockaddr* sa, socklen_t* len) {
    struct sockaddr_in6 s6{};
    struct sockaddr_in  s4{};
    const void* src = &s4;
    size_t n = sizeof(s4);
    if (fd < 1000) {
        s4.sin_family = AF_INET;
        s4.sin_addr.s_addr = htonl(0x0A000000u + static_cast<uint32_t>(fd));
    } else {
        s6.sin6_family = AF_INET6;
        uint8_t* a = s6.sin6_addr.s6_addr;
        if (fd < 2000) {
            a[0] = 0x20; a[1] = 0x01; a[2] = 0x0d; a[3] = 0xb8;
            a[7] = static_cast<uint8_t>((fd - 1000) / 10);
            a[15] = static_cast<uint8_t>(fd % 10 + 1);
        } else {
            a[10] = a[11] = 0xff;
            a[12] = 10; a[15] = static_cast<uint8_t>(fd - 2000);
        }
        src = &s6;
        n = sizeof(s6);
    }
    std::memcpy(sa, src, n < *len ? n : *len);   // a short buffer truncates, as the real call does
    *len = static_cast<socklen_t>(n);
    return 0;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char* f) {
    auto it = g_hdr.find(f); return it == g_hdr.end() ? 0 : it->second.size();
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char* f, char* val, size_t len) {
    auto it = g_hdr.find(f); if (it == g_hdr.end() || !len) return ESP_FAIL;
    std::snprintf(val, len, "%s", it->second.c_str()); return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t*, const char* s) { g_status = s; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char* f, const char* v) {
    if (!std::strcmp(f, "Retry-After")) g_retry_after = v;
    if (g_resp_hdr.count(f)) g_resp_hdr_dups++;
    g_resp_hdr[f] = v;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t*, const char* s) { g_resp = s; return ESP_OK; }
int httpd_req_recv(httpd_req_t*, char* buf, size_t len) {
    const size_t n = g_body.size() < len ? g_body.size() : len;
    std::memcpy(buf, g_body.data(), n);
    g_body.erase(0, n);
    return static_cast<int>(n);
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) { g_routes[u->uri] = *u; return ESP_OK; }

void ws_server_set_api_token(const char* t) { g_ws_token = t ? t : ""; }
void ws_server_fd_deauth_all() { g_ws_socket_authed = false; }
void ws_server_close_unauthed() {}
const char* net_discovery_hostname() { return "zhac"; }
uint32_t zap_setup_secs_left() { return g_setup_left; }

extern "C" int __wrap_printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_serial += buf;
    return n;
}

// POST `body` to `uri` from peer `fd`, the way the hub's own page would; returns the HTTP status.
static std::string post(const char* uri, int fd, const std::string& body,
                        const char* type = "application/json", const std::string& key = "") {
    g_hdr = {{"Host", "192.168.1.5"}, {"Origin", "http://192.168.1.5"}};
    if (type) g_hdr["Content-Type"] = type;
    if (!key.empty()) g_hdr["X-Api-Key"] = key;
    g_body = body;
    g_status = "200 OK";
    g_resp.clear();
    g_retry_after.clear();
    g_resp_hdr.clear();
    g_resp_hdr_dups = 0;
    const httpd_uri_t& u = g_routes.at(uri);
    httpd_req_t r{body.size(), u.user_ctx, fd};
    u.handler(&r);
    return g_status;
}
static std::string login(int fd, const char* pw) {
    return post("/api/auth/login", fd, std::string("{\"password\":\"") + pw + "\"}");
}
static std::string resp_token() {   // the token in the last {"ok":true,"token":"..."} answer
    const size_t at = g_resp.find("\"token\":\"");
    return at == std::string::npos ? "" : g_resp.substr(at + 9, 32);
}
static std::string boot() {          // a reboot: auth_init again, returning what it printed
    g_serial.clear();
    auth_init();
    return g_serial;
}
static std::string current_token() { char t[33]; auth_token_copy(t, sizeof(t)); return t; }

// ── checks ──────────────────────────────────────────────────────────────────
static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::fprintf(stdout, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    const char* kWrong = "00000000000000000000000000000000";
    auth_init();                                  // nothing stored, default off
    auth_register(nullptr);
    // The first token is drawn at boot, before the radio runs (HS-15). A chip with a radio (the S31) has
    // true entropy then only with the bootloader's source turned on again; the P4 keeps its source on.
#if SOC_WIFI_SUPPORTED || SOC_IEEE802154_SUPPORTED || SOC_BT_SUPPORTED
    expect("radio chip: the boot token is drawn with the entropy source on", g_last_fill_entropy);
    expect("  which is off again before the radio starts", !g_entropy_on && g_entropy_calls == 2);
#else
    expect("no radio: the entropy source is left as it is", g_entropy_calls == 0);
#endif
    expect("sign-in starts off", !auth_enabled());
    expect("no token installed while off", g_ws_token.empty());

    g_ws_socket_authed = true;                    // a browser that connected while sign-in was off
    auth_set_enabled(true);
    expect("turning sign-in on installs the token", g_ws_token.size() == 32);
    expect("turning sign-in on signs that socket out", !g_ws_socket_authed);
    expect("a wrong token is still refused", !auth_check_token(kWrong, 5));

    g_ws_socket_authed = true;                    // it signed in with the token
    auth_set_enabled(true);                       // saving "on" again is no transition
    expect("saving on again keeps signed-in sockets", g_ws_socket_authed);

    // One peer guessing must not lock out another peer's valid login.
    std::string good = g_ws_token;
    for (int i = 0; i < 6; i++) auth_check_token(kWrong, 7);
    expect("the guessing peer is locked out", !auth_check_token(good.c_str(), 7));
    expect("another peer still signs in", auth_check_token(good.c_str(), 8));

    auth_set_enabled(false);
    expect("turning sign-in off removes the token", g_ws_token.empty());

    // ── first claim (HS-05, HS-07) ──
    auth_set_enabled(true);
    std::string out = boot();
    expect("no password yet: the console shows the token", out.find(current_token()) != std::string::npos);
    g_setup_left = 600;
    const std::string boot_token = current_token();
    expect("set-up refuses a body that is not JSON",
           post("/api/auth/setup", 9, "{\"password\":\"hunter2hunter2\"}", "text/plain") ==
               "415 Unsupported Media Type");
    expect("  and sets no password", !auth_password_is_set());
    expect("set-up as JSON sets the password",
           post("/api/auth/setup", 9, "{\"password\":\"hunter2hunter2\"}") == "200 OK" && auth_password_is_set());
    expect("  and answers with a fresh token, not the one the console showed",
           resp_token().size() == 32 && resp_token() != boot_token && resp_token() == current_token());
    expect("  which the WebSocket server now expects", g_ws_token == current_token());
    expect("the console's token no longer signs in", !auth_check_token(boot_token.c_str(), 9));
    g_setup_left = 0;

    out = boot();
    good = current_token();
    expect("password set: the console hides the token",
           out.find(good) == std::string::npos && out.find("(hidden)") != std::string::npos);
    expect("the password signs in", login(10, "hunter2hunter2") == "200 OK" && resp_token() == good);
    expect("login refuses a body that is not JSON",
           post("/api/auth/login", 10, "{\"password\":\"hunter2hunter2\"}", "text/plain") ==
               "415 Unsupported Media Type");

    // ── who gets the full /api/status (HS-08) ──
    auto status_req = [](int fd, const std::string& key) {
        g_hdr = {{"Host", "192.168.1.5"}};
        if (!key.empty()) g_hdr["X-Api-Key"] = key;
        httpd_req_t r{0, nullptr, fd};
        return auth_request_signed_in(&r);
    };
    bool none_ok = true;
    for (int i = 0; i < 10; i++) none_ok = none_ok && !status_req(13, "");
    expect("sign-in on: a status request without a key is not signed in", none_ok);
    expect("  and asking without a key is no guess: the peer is not locked out", status_req(13, good));
    expect("  a wrong key is not signed in", !status_req(14, kWrong));
    expect("  the token is", status_req(14, good));

    // ── every answer: no MIME sniffing, no Referer (HS-14) ──
    auto sec = [] {
        return g_resp_hdr["X-Content-Type-Options"] == "nosniff" && g_resp_hdr["Referrer-Policy"] == "no-referrer" &&
               g_resp_hdr_dups == 0;
    };
    login(10, "hunter2hunter2");
    expect("a login answer says nosniff and no-referrer, once", sec());
    post("/api/auth/setup", 11, "{\"password\":\"another-one-1\"}");
    expect("  a refused set-up too", g_status != "200 OK" && sec());
    expect("  a gated route's 401 too", post("/api/auth/password", 12, "{}") == "401 Unauthorized" && sec());
    expect("  and its answer once signed in",
           post("/api/auth/password", 12, "{\"x\":1}", "application/json", good) == "400 Bad Request" && sec());

    // ── lockout answers (HS-06) and the token on the console (HS-07) ──
    for (int i = 0; i < 5; i++) login(30, "wrong-password");
    expect("a locked-out peer gets 429, not 401", login(30, "hunter2hunter2") == "429 Too Many Requests");
    expect("  with Retry-After", g_retry_after == "60");
    out = boot();
    expect("after a password lockout the next boot shows the token", out.find(good) != std::string::npos);
    out = boot();
    expect("  that boot only", out.find(good) == std::string::npos);

    // ── addresses (HS-06) ──
    g_now_us += 61 * 1000000LL;                   // every earlier failure leaves the window
    for (int i = 0; i < 5; i++) auth_check_token(kWrong, 40);
    for (int fd = 41; fd <= 48; fd++) {           // more peers than buckets, each one newer
        g_now_us += 1000;
        auth_check_token(kWrong, fd);
    }
    expect("other peers' failures do not free a locked-out peer", !auth_check_token(good.c_str(), 40));
    expect("a full table does not refuse a new peer's valid token", auth_check_token(good.c_str(), 49));
    expect("  nor a new peer's right password", login(50, "hunter2hunter2") == "200 OK");

    g_now_us += 61 * 1000000LL;
    for (int fd = 1000; fd < 1005; fd++) auth_check_token(kWrong, fd);   // one /64, five addresses
    expect("an IPv6 /64 is one peer", !auth_check_token(good.c_str(), 1005));
    expect("  another /64 is not locked", auth_check_token(good.c_str(), 1010));
    for (int i = 0; i < 5; i++) auth_check_token(kWrong, 2051);   // ::ffff:10.0.0.51
    expect("an IPv4-mapped peer counts as its IPv4 address", !auth_check_token(good.c_str(), 51));

    g_now_us += 61 * 1000000LL;
    for (int fd = 60; fd < 65; fd++)
        for (int i = 0; i < 4; i++) login(fd, "wrong-password");   // 20 wrong, no peer locked
    expect("twenty wrong passwords a minute lock the password hub-wide",
           login(65, "hunter2hunter2") == "429 Too Many Requests");
    expect("  but not the token", auth_check_token(good.c_str(), 65));
    g_now_us += 61 * 1000000LL;
    expect("  for a minute", login(65, "hunter2hunter2") == "200 OK");

    std::fprintf(stdout, g_fail ? "\n%d FAILURE(S)\n" : "\nauth OK\n", g_fail);
    return g_fail;
}
