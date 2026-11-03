// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The cross-site filter's rules (review 2026-10-24 HS-01), pure so they can be
// host-tested; auth.cpp applies them to every API request and the WebSocket
// upgrade.
//
// A browser lets any page it shows reach the LAN: a WebSocket upgrade and a
// simple POST need no CORS preflight, and DNS rebinding makes a foreign page
// same-origin with the hub. With sign-in off, such a page could drive every
// command, up to an OTA. Browsers name the page in `Origin` on every
// cross-origin request and on every POST/PUT/DELETE, so (auth.cpp applies
// both while sign-in is off, and always to the first claim of a fresh hub):
//   - an Origin must be the hub's own: its host equal to Host's (a dev server
//     proxying to the hub passes, its Origin and Host are both localhost);
//   - Host must be the hub's name, <name>.local, an IP literal or a loopback
//     name: a rebinding page always arrives with its own domain in Host.
// curl and scripts send no Origin and are not affected by the first rule.
#pragma once

#include <cstddef>
#include <cstring>
#include <strings.h>

// The host part of a Host header or an Origin: "zhac.local" for
// "http://zhac.local:8080", "[fe80::1]" for "[fe80::1]:80", without a
// trailing dot. False when it is empty or does not fit `cap`.
inline bool req_host_part(const char* s, char* out, size_t cap) {
    if (!s || !out || cap == 0) return false;
    if (const char* p = std::strstr(s, "://")) s = p + 3;
    size_t n = 0;
    if (*s == '[') {                         // IPv6 literal: through the bracket
        const char* e = std::strchr(s, ']');
        if (!e) return false;
        n = static_cast<size_t>(e - s) + 1;
    } else {
        while (s[n] && s[n] != ':' && s[n] != '/') n++;
        if (n > 1 && s[n - 1] == '.') n--;   // "zhac.local." is "zhac.local"
    }
    if (n == 0 || n >= cap) return false;
    std::memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

inline bool req_is_ip_literal(const char* h) {
    if (!h || !*h) return false;
    if (h[0] == '[') return true;            // IPv6 (req_host_part keeps the brackets)
    int dots = 0;
    for (const char* p = h; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return false;
    }
    return dots == 3;
}

inline bool req_is_loopback(const char* h) {
    return !strcasecmp(h, "localhost") || !strcasecmp(h, "127.0.0.1") || !strcasecmp(h, "[::1]");
}

// A request without an Origin (a script, or a browser's same-origin GET) passes.
inline bool req_origin_ok(const char* origin, const char* host) {
    if (!origin || !*origin) return true;
    char o[96], h[96];
    if (!req_host_part(origin, o, sizeof(o))) return false;
    return host && req_host_part(host, h, sizeof(h)) && !strcasecmp(o, h);
}

// `name` is the hub's mDNS hostname ("zhac"). A request without a Host
// (HTTP/1.0 tooling) passes.
inline bool req_host_ok(const char* host, const char* name) {
    if (!host || !*host) return true;
    char h[96];
    if (!req_host_part(host, h, sizeof(h))) return false;
    if (req_is_ip_literal(h) || req_is_loopback(h)) return true;
    if (!name || !*name) return false;
    const size_t n = std::strlen(name);
    if (strncasecmp(h, name, n) != 0) return false;
    return h[n] == '\0' || !strcasecmp(h + n, ".local");
}
