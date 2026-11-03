// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// auth.cpp — see auth.h. Ported from zhac-net-core (main.cpp auth section,
// rest_ops.cpp login/setup/password); keep the two in step.
#include "auth.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include <lwip/sockets.h>   // getpeername, for the per-peer lockout

#include "ArduinoJson.h"
#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_discovery.h"
#include "nvs.h"
#include "psa/crypto.h"     // IDF v6 (mbedtls 4): mbedtls/sha256.h is private
#include "req_filter.h"
#include "sdkconfig.h"
#include "sec_headers.h"
#include "soc/soc_caps.h"
#include "ws_server.h"
#include "zap_setup_window.h"

static const char* TAG = "auth";

namespace {

char s_token[33] = {};
bool s_enabled = false;
bool s_storage_error = false;   // zhac_auth namespace could not be opened at boot

// ── Per-peer lockout (net-core CC-F8): 5 failures in 60 s blocks that peer ──
constexpr uint8_t  kFailLimit   = 5;
constexpr uint32_t kFailWindowMs = 60 * 1000;
constexpr uint8_t  kBuckets     = 8;
struct Bucket {
    uint32_t ip;
    uint32_t ts[kFailLimit];
    uint8_t  head;
    uint32_t last_use_ms;   // 0 = free
};
Bucket            s_buckets[kBuckets] = {};
SemaphoreHandle_t s_fail_mtx = nullptr;

// Review 2026-10-24 HS-06: a bucket that holds a failure inside the window is
// never evicted (cycling through nine source addresses used to wipe a peer's
// lockout). With all of them live, a new peer's failures go uncounted rather
// than refusing it: a token guess is hopeless (128 bits), and wrong passwords
// also count against one hub-wide budget, which bounds the guessing and the
// KDF runs on the httpd task.
constexpr uint8_t kHubPwFailLimit = 20;
uint32_t s_hub_pw_fail[kHubPwFailLimit] = {};
uint8_t  s_hub_pw_head = 0;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// The key a peer's failures count under: its IPv4 address, or for IPv6 its
// /64 (one host owns a whole prefix), folded to 32 bits.
uint32_t peer_ipv4_fd(int fd) {
    if (fd < 0) return 0;
    struct sockaddr_in6 sa;
    socklen_t sl = sizeof(sa);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&sa), &sl) != 0) return 0;
    if (sa.sin6_family == AF_INET) return reinterpret_cast<struct sockaddr_in*>(&sa)->sin_addr.s_addr;
    const uint8_t* a = sa.sin6_addr.s6_addr;
    static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    uint32_t k, hi;
    if (memcmp(a, kMapped, sizeof(kMapped)) == 0) {   // an IPv4 peer on a dual-stack socket
        memcpy(&k, a + 12, sizeof(k));
        return k;
    }
    memcpy(&k, a, sizeof(k));
    memcpy(&hi, a + 4, sizeof(hi));
    return k ^ hi;
}

uint32_t peer_ipv4(httpd_req_t* req) { return peer_ipv4_fd(httpd_req_to_sockfd(req)); }

Bucket* bucket_find(uint32_t ip) {
    for (auto& b : s_buckets) if (b.last_use_ms != 0 && b.ip == ip) return &b;
    return nullptr;
}

bool bucket_live(const Bucket& b, uint32_t now) {
    if (b.last_use_ms == 0) return false;
    for (uint32_t t : b.ts) if (t != 0 && (now - t) < kFailWindowMs) return true;
    return false;
}

// A free or expired bucket, or nullptr when all of them hold live failures.
Bucket* bucket_spare(uint32_t now) {
    for (auto& e : s_buckets) if (!bucket_live(e, now)) return &e;
    return nullptr;
}

void record_failure(uint32_t ip) {
    if (!s_fail_mtx || xSemaphoreTake(s_fail_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    const uint32_t now = now_ms();
    Bucket* b = bucket_find(ip);
    if (!b && (b = bucket_spare(now)) != nullptr) {
        *b = Bucket{};
        b->ip = ip;
    }
    if (b) {   // none spare: uncounted (see above)
        b->last_use_ms = now;
        b->ts[b->head] = now;
        b->head = (b->head + 1) % kFailLimit;
    }
    xSemaphoreGive(s_fail_mtx);
}

bool locked(uint32_t ip) {
    if (!s_fail_mtx || xSemaphoreTake(s_fail_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const uint32_t now = now_ms();
    uint8_t recent = 0;
    const Bucket* b = bucket_find(ip);
    if (b)
        for (uint32_t t : b->ts) if (t != 0 && (now - t) < kFailWindowMs) recent++;
    xSemaphoreGive(s_fail_mtx);
    return recent >= kFailLimit;
}

bool hub_pw_locked() {
    const uint32_t now = now_ms();
    uint8_t recent = 0;
    for (uint32_t t : s_hub_pw_fail) if (t != 0 && (now - t) < kFailWindowMs) recent++;
    return recent >= kHubPwFailLimit;
}

void hub_pw_failed() {
    s_hub_pw_fail[s_hub_pw_head] = now_ms();
    s_hub_pw_head = (s_hub_pw_head + 1) % kHubPwFailLimit;
}

// Constant-time: no early exit on the first differing byte. `c` must hold 32.
bool token_matches(const char* c) {
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= static_cast<uint8_t>(c[i]) ^ static_cast<uint8_t>(s_token[i]);
    return diff == 0;
}

bool check_request(httpd_req_t* req) {
    if (!s_enabled) return true;
    const uint32_t ip = peer_ipv4(req);
    if (locked(ip)) return false;
    char key[64] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Api-Key", key, sizeof(key)) != ESP_OK ||
        strlen(key) != 32 || !token_matches(key)) {
        record_failure(ip);
        return false;
    }
    return true;
}

// ── Admin password: salted, iterated SHA-256 in NVS (net-core's scheme) ─────
constexpr int    kKdfRounds = 8192;
constexpr size_t kPwMin = 8, kPwMax = 63;
uint8_t s_salt[16];
uint8_t s_hash[32];
bool    s_pw_set = false;

bool kdf(const char* pw, const uint8_t salt[16], uint8_t out[32]) {
    const size_t n = strlen(pw);
    if (n > kPwMax) return false;
    uint8_t buf[32 + 16 + kPwMax];     // H(i-1) | salt | pw
    uint8_t h[32];
    size_t olen = 0;
    memcpy(buf + 32, salt, 16);
    memcpy(buf + 48, pw, n);
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf + 32, 16 + n, h, sizeof(h), &olen);
    for (int i = 1; st == PSA_SUCCESS && i < kKdfRounds; i++) {
        memcpy(buf, h, 32);
        st = psa_hash_compute(PSA_ALG_SHA_256, buf, 32 + 16 + n, h, sizeof(h), &olen);
    }
    if (st != PSA_SUCCESS || olen != 32) { memset(out, 0, 32); return false; }
    memcpy(out, h, 32);
    return true;
}

bool password_store(const char* pw) {
    const size_t n = pw ? strlen(pw) : 0;
    if (n < kPwMin || n > kPwMax) return false;
    uint8_t salt[16], hash[32];
    esp_fill_random(salt, sizeof(salt));
    if (!kdf(pw, salt, hash)) return false;            // never persist a garbage hash
    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, "pw_salt", salt, sizeof(salt));
    if (e == ESP_OK) e = nvs_set_blob(h, "pw_hash", hash, sizeof(hash));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return false;
    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_hash, hash, sizeof(hash));
    const bool was = s_pw_set;
    s_pw_set = true;
    ESP_LOGW(TAG, "admin password %s", was ? "changed" : "set");
    return true;
}

// A lockout on the password is how an owner who lost it asks for the token
// (review 2026-10-24 HS-07): the next boot prints it on the serial console,
// which only someone at the hub can read. Remembered once per boot.
void note_lockout(uint32_t ip) {
    static bool noted = false;
    if (noted || s_storage_error || !locked(ip)) return;
    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "show_tok", 1) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    noted = true;
}

enum class PwCheck { OK, WRONG, LOCKED };

PwCheck password_check(httpd_req_t* req, const char* pw) {
    const uint32_t ip = peer_ipv4(req);
    if (locked(ip) || hub_pw_locked()) return PwCheck::LOCKED;
    uint8_t h[32];
    bool ok = s_pw_set && pw && pw[0] && kdf(pw, s_salt, h);
    if (ok) {
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= h[i] ^ s_hash[i];
        ok = diff == 0;
    }
    if (ok) return PwCheck::OK;
    record_failure(ip);
    hub_pw_failed();
    note_lockout(ip);
    return PwCheck::WRONG;
}

void new_token(char out[33]) {
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", rnd[i]);
}

// A token drawn at boot, before the radio runs. On a chip with a radio (the
// S31) the RNG has true entropy only while the radio or the bootloader's
// entropy source is on, and the bootloader turns its source off before the app
// starts: turn it on for these bytes, as IDF's nvs_sec_provider does for its
// first-boot key. Nothing uses the ADC this early. The P4 has no radio and
// keeps the source on; it is left alone there (review HS-15).
void boot_token(char out[33]) {
#if SOC_WIFI_SUPPORTED || SOC_IEEE802154_SUPPORTED || SOC_BT_SUPPORTED
    bootloader_random_enable();
    new_token(out);
    bootloader_random_disable();
#else
    new_token(out);
#endif
}

bool persist_token(const char* t) {
    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, "token", t);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// ── REST: 401 wrapper and the three auth routes ─────────────────────────────
using Handler = esp_err_t (*)(httpd_req_t*);

esp_err_t reply(httpd_req_t* req, const char* status, const char* body) {
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t gated(httpd_req_t* req) {
    sec_headers(req);
    if (!auth_request_allowed(req)) return auth_reply_cross_site(req);
    if (!check_request(req)) return reply(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
    return reinterpret_cast<Handler>(req->user_ctx)(req);
}

// 429 while a peer, or the hub as a whole, is locked out of the password.
esp_err_t reply_locked(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Retry-After", "60");
    return reply(req, "429 Too Many Requests", "{\"error\":\"too_many_attempts\"}");
}

// Read a small JSON body; false (already answered) if it is missing or too big.
// It must be sent as JSON: a page of another site cannot send that type without
// a CORS preflight the hub never grants (review 2026-10-24 HS-05).
bool read_json(httpd_req_t* req, JsonDocument& doc, size_t cap) {
    char buf[320];
    char ct[48] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/json", 16) != 0) {
        reply(req, "415 Unsupported Media Type", "{\"error\":\"json_required\"}");
        return false;
    }
    if (req->content_len == 0 || req->content_len >= cap || req->content_len >= sizeof(buf)) {
        reply(req, "400 Bad Request", "{\"error\":\"invalid request\"}");
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { reply(req, "400 Bad Request", "{\"error\":\"invalid request\"}"); return false; }
        got += static_cast<size_t>(r);
    }
    if (deserializeJson(doc, buf, got)) {
        reply(req, "400 Bad Request", "{\"error\":\"invalid request\"}");
        return false;
    }
    return true;
}

esp_err_t send_token(httpd_req_t* req, const char* token) {
    char rsp[64];
    snprintf(rsp, sizeof(rsp), "{\"ok\":true,\"token\":\"%s\"}", token);
    return reply(req, nullptr, rsp);
}

esp_err_t handle_login(httpd_req_t* req) {
    sec_headers(req);
    if (!auth_request_allowed(req)) return auth_reply_cross_site(req);
    JsonDocument doc;
    if (!read_json(req, doc, 192)) return ESP_OK;
    if (!doc["password"].is<const char*>()) return reply(req, "400 Bad Request", "{\"error\":\"invalid request\"}");
    if (!s_enabled) return reply(req, nullptr, "{\"ok\":true,\"auth\":false}");
    if (!s_pw_set) return reply(req, "409 Conflict", "{\"error\":\"setup_required\"}");
    switch (password_check(req, doc["password"].as<const char*>())) {
    case PwCheck::LOCKED: return reply_locked(req);
    case PwCheck::WRONG:  return reply(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
    case PwCheck::OK:     break;
    }
    return send_token(req, s_token);
}

// Open only while no password exists AND the hub was powered on less than
// kZapSetupWindowS ago (zap_setup_window.h): the first visitor to a fresh hub
// claims it, but only someone who can power-cycle the hub can reopen that.
esp_err_t handle_setup(httpd_req_t* req) {
    // A foreign page, rebinding included, must not claim a fresh hub and read
    // its token from the answer (review HS-05): both rules, sign-in on or off.
    sec_headers(req);
    if (!auth_request_from_hub_page(req)) return auth_reply_cross_site(req);
    if (s_storage_error) return reply(req, "503 Service Unavailable", "{\"error\":\"storage_error\"}");
    if (s_pw_set) return reply(req, "403 Forbidden", "{\"error\":\"already_set\"}");
    if (zap_setup_secs_left() == 0) return reply(req, "403 Forbidden", "{\"error\":\"setup_closed\"}");
    JsonDocument doc;
    if (!read_json(req, doc, 192)) return ESP_OK;
    const char* pw = doc["password"] | (const char*)nullptr;
    if (!pw || strlen(pw) < kPwMin || strlen(pw) > kPwMax)
        return reply(req, "400 Bad Request", "{\"error\":\"password must be 8-63 characters\"}");
    if (!password_store(pw)) return reply(req, "500 Internal Server Error", "{\"error\":\"persist failed\"}");
    // The token of a hub without a password is printed on the serial console at
    // boot (HS-07): a fresh one goes to the browser that claims the hub.
    char fresh[33];
    if (!auth_rotate_token(fresh, sizeof(fresh)))
        return reply(req, "500 Internal Server Error", "{\"error\":\"rotate failed\"}");
    return send_token(req, fresh);
}

// Needs the token AND the current password, then rotates the token so every
// other browser has to log in again.
esp_err_t handle_password(httpd_req_t* req) {
    JsonDocument doc;
    if (!read_json(req, doc, 320)) return ESP_OK;
    const char* npw = doc["new"] | (const char*)nullptr;
    if (!npw) return reply(req, "400 Bad Request", "{\"error\":\"invalid request\"}");
    if (s_pw_set) {
        switch (password_check(req, doc["current"] | "")) {
        case PwCheck::LOCKED: return reply_locked(req);
        case PwCheck::WRONG:  return reply(req, "401 Unauthorized", "{\"error\":\"wrong_password\"}");
        case PwCheck::OK:     break;
        }
    }
    if (strlen(npw) < kPwMin || strlen(npw) > kPwMax)
        return reply(req, "400 Bad Request", "{\"error\":\"password must be 8-63 characters\"}");
    if (!password_store(npw)) return reply(req, "500 Internal Server Error", "{\"error\":\"persist failed\"}");
    char fresh[33];
    if (!auth_rotate_token(fresh, sizeof(fresh)))
        return reply(req, "500 Internal Server Error", "{\"error\":\"rotate failed\"}");
    return send_token(req, fresh);
}

}  // namespace

void auth_init() {
    if (!s_fail_mtx) s_fail_mtx = xSemaphoreCreateMutex();
    if (psa_crypto_init() != PSA_SUCCESS) ESP_LOGE(TAG, "psa_crypto_init failed -- password login unavailable");
    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READWRITE, &h) != ESP_OK) {
        // Storage fault. Fail CLOSED: sign-in stays on with a token that lives
        // only in RAM and is printed on the serial console -- the one recovery
        // path -- and no password can be set until storage works again. The
        // old behaviour (auth off) turned a broken flash into an open hub.
        s_storage_error = true;
        s_enabled = true;
        s_pw_set = false;
        boot_token(s_token);
        ws_server_set_api_token(s_token);
        ESP_LOGE(TAG, "nvs_open(zhac_auth) failed -- STORAGE ERROR: sign-in forced on, "
                      "password set-up refused until storage is reset");
        printf("\n*** ZHAC auth storage unreadable -- serial-only token for this boot: %s ***\n"
               "    Sign in with it (Login -> \"Use API token\"), then reset storage from Settings.\n\n",
               s_token);
        fflush(stdout);
        return;
    }
#if CONFIG_ZHAC_API_AUTH_DEFAULT_ENABLED
    uint8_t en = 1;
#else
    uint8_t en = 0;
#endif
    nvs_get_u8(h, "enabled", &en);   // a stored choice always wins
    s_enabled = en != 0;
    size_t len = sizeof(s_token);
    if (nvs_get_str(h, "token", s_token, &len) != ESP_OK || !s_token[0]) {
        boot_token(s_token);
        nvs_set_str(h, "token", s_token);
        nvs_commit(h);
    }
    size_t sl = sizeof(s_salt), hl = sizeof(s_hash);
    s_pw_set = nvs_get_blob(h, "pw_salt", s_salt, &sl) == ESP_OK && sl == sizeof(s_salt) &&
               nvs_get_blob(h, "pw_hash", s_hash, &hl) == ESP_OK && hl == sizeof(s_hash);
    uint8_t show = 0;   // a password lockout asked for the token (note_lockout)
    if (nvs_get_u8(h, "show_tok", &show) == ESP_OK && nvs_erase_key(h, "show_tok") == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ws_server_set_api_token(s_enabled ? s_token : nullptr);
    if (s_enabled) {
        // Serial only, never the log ring (served by /api/logs): the recovery
        // path when the password is lost. Physical access already implies
        // full access on a board without flash encryption. In full only while
        // it is the way in (review 2026-10-24 HS-07): no password yet, or a
        // lockout on the password asked for it. Otherwise a fingerprint, so a
        // boot log shared for support does not carry it.
        if (!s_pw_set || show) {
            printf("\n*** ZHAC API auth ENABLED -- token (serial-only): %s ***\n"
                   "    Lost the admin password? Sign in with this token (Login -> \"Use API token\").\n\n",
                   s_token);
        } else {
            printf("\n*** ZHAC API auth ENABLED -- token ...%s (hidden) ***\n"
                   "    Lost the admin password? Enter a wrong one five times, then power-cycle the hub:\n"
                   "    the next boot prints the token here.\n\n",
                   s_token + 28);
        }
        fflush(stdout);
    }
    ESP_LOGI(TAG, "API auth %s, admin password %s", s_enabled ? "on" : "off",
             s_pw_set ? "set" : "not set (first visit sets it)");
    if (s_enabled && !s_pw_set)
        ESP_LOGW(TAG, "no admin password: the web UI can set one for %" PRIu32 " min after power-on, "
                 "then a power cycle reopens that", kZapSetupWindowS / 60);
}

uint32_t auth_setup_secs_left() { return (s_enabled && !s_pw_set) ? zap_setup_secs_left() : 0; }

bool auth_enabled() { return s_enabled; }
bool auth_storage_error() { return s_storage_error; }
bool auth_password_is_set() { return s_pw_set; }

void auth_set_enabled(bool en) {
    const bool was = s_enabled;
    s_enabled = en;
    nvs_handle_t h;
    if (nvs_open("zhac_auth", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ws_server_set_api_token(en ? s_token : nullptr);
    // Sockets opened while sign-in was off were let in without a token; the
    // dispatcher trusts that flag, so they must sign in like everyone else.
    if (en && !was) ws_server_fd_deauth_all();
    // The other way, every socket is trusted from now on: those that never
    // signed in may be another site's page (the upgrade filter only runs while
    // sign-in is off), so they go. The owner's signed-in page stays.
    if (!en && was) ws_server_close_unauthed();
    ESP_LOGW(TAG, "API auth %s", en ? "enabled" : "DISABLED -- anyone on the LAN has full access");
}

size_t auth_token_copy(char* out, size_t cap) {
    if (!out || !cap) return 0;
    snprintf(out, cap, "%s", s_token);
    return strlen(out);
}

bool auth_rotate_token(char* out, size_t cap) {
    if (cap < 33) return false;
    char fresh[33];
    new_token(fresh);
    if (!persist_token(fresh)) return false;
    memcpy(s_token, fresh, sizeof(fresh));
    if (s_enabled) {
        ws_server_set_api_token(s_token);
        ws_server_fd_deauth_all();   // sessions on the old token must re-auth
    }
    memcpy(out, fresh, sizeof(fresh));
    ESP_LOGW(TAG, "API token rotated");
    return true;
}

bool auth_check_token(const char* token, int fd) {
    // The real token even with sign-in off: only a socket that never signed in
    // asks then (one opened while sign-in is off starts signed in).
    // Failures count against the socket's peer, as on REST: one shared bucket
    // let any peer's five wrong guesses refuse everyone's valid token (review F31).
    const uint32_t ip = peer_ipv4_fd(fd);
    if (locked(ip)) return false;
    if (!token || strlen(token) != 32 || !token_matches(token)) { record_failure(ip); return false; }
    return true;
}

// At most one line per 10 s, and without the header values: a page that keeps
// trying must not fill the log, nor write its own text into it.
static void log_refusal(const char* why) {
    static int64_t s_last_us = 0;
    const int64_t now = esp_timer_get_time();
    if (s_last_us && now - s_last_us < 10 * 1000000LL) return;
    s_last_us = now;
    ESP_LOGW(TAG, "refused a request: %s", why);
}

bool auth_request_from_hub_page(httpd_req_t* req) {
    char host[96] = {}, origin[128] = {};
    const size_t hl = httpd_req_get_hdr_value_len(req, "Host");
    const size_t ol = httpd_req_get_hdr_value_len(req, "Origin");
    if (hl >= sizeof(host) || ol >= sizeof(origin)) {             // cut short would not parse right
        log_refusal("oversized Host or Origin");
        return false;
    }
    if (hl) httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (ol) httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (!req_origin_ok(origin, host)) {
        log_refusal("sent by a page of another site, or through a proxy that rewrites Host");
        return false;
    }
    // The configured name or the one mDNS gave the hub after a conflict ("zhac-2").
    if (!req_host_ok(host, net_discovery_hostname()) && !req_host_ok(host, CONFIG_ZHAC_MDNS_HOSTNAME)) {
        log_refusal("a host name other than the hub's -- open it by its IP address or its .local name");
        return false;
    }
    return true;
}

// With sign-in on the token is the gate: a page of another site cannot read
// it, and a reverse proxy that rewrites Host or a router's DNS name keep
// working. The filter guards what is open without it.
bool auth_request_allowed(httpd_req_t* req) {
    return s_enabled || auth_request_from_hub_page(req);
}

bool auth_request_signed_in(httpd_req_t* req) {
    if (!s_enabled) return true;
    if (httpd_req_get_hdr_value_len(req, "X-Api-Key") == 0) return false;   // asked without a key
    return check_request(req);
}

esp_err_t auth_reply_cross_site(httpd_req_t* req) {
    return reply(req, "403 Forbidden", "{\"error\":\"cross_site\"}");
}

esp_err_t auth_register_uri(httpd_handle_t hd, const httpd_uri_t* u) {
    httpd_uri_t g = *u;
    g.user_ctx = reinterpret_cast<void*>(u->handler);
    g.handler  = gated;
    return httpd_register_uri_handler(hd, &g);
}

void auth_register(httpd_handle_t hd) {
    httpd_uri_t u{};
    u.method = HTTP_POST;
    u.uri = "/api/auth/login";    u.handler = handle_login; httpd_register_uri_handler(hd, &u);
    u.uri = "/api/auth/setup";    u.handler = handle_setup; httpd_register_uri_handler(hd, &u);
    u.uri = "/api/auth/password"; u.handler = handle_password; auth_register_uri(hd, &u);
}
