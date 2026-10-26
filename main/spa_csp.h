// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The web UI's Content-Security-Policy, which spa_serve.cpp sends with index.html (a <meta> CSP cannot
// cover the WebSocket). 'unsafe-inline' styles: CodeMirror injects them. api.github.com: the OTA page
// lists the published releases from the browser.
//
// The WebSocket may go to this host only: bare `ws: wss:` allowed any host (review HS-14). CSP3 browsers
// read 'self' that way; ws:// and wss:// of the Host the page was asked for are added for older ones,
// and only when that Host is a plain name or IPv4 address, with or without a port.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

inline constexpr char kSpaCspHead[] = "default-src 'self'; connect-src 'self'";
inline constexpr char kSpaCspTail[] =
    " https://api.github.com; script-src 'self'; "
    "style-src 'self' 'unsafe-inline'; img-src 'self' data:; base-uri 'none'; "
    "frame-ancestors 'none'";
inline constexpr size_t kSpaCspHostMax = 63;
inline constexpr size_t kSpaCspMax = sizeof(kSpaCspHead) + sizeof(kSpaCspTail) + 2 * (7 + kSpaCspHostMax);

inline bool spa_csp_host_ok(const char* h) {
    if (!h || !*h || std::strlen(h) > kSpaCspHostMax) return false;
    for (const char* p = h; *p; p++)
        if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '.' && *p != '-' && *p != ':') return false;
    return true;
}

// The policy for a page asked for as `host` (the Host header; null or "" when there is none).
inline void spa_csp(const char* host, char* out, size_t cap) {
    if (spa_csp_host_ok(host)) std::snprintf(out, cap, "%s ws://%s wss://%s%s", kSpaCspHead, host, host, kSpaCspTail);
    else                       std::snprintf(out, cap, "%s%s", kSpaCspHead, kSpaCspTail);
}
