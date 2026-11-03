// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Follow an update URL's redirects before the download starts.
//
// zhac-net-core/main/ota_redirect.h is a copy (review 2026-10-24 F-04); keep the
// two in step. The walk reads the Location itself (ota_follow) and keeps
// everything in it: esp_http_client_get_url(), which it asked before, leaves the
// query out and so cut the signature off a signed download URL (GitHub's
// release assets redirect to one).
//
// esp_https_ota follows redirects itself, but on IDF 6.0 it takes one to
// http:// (a certificate bundle does not stop that) and its connect loop never
// checks max_redirection_count, so a redirect loop held the updater forever.
// Here every hop must stay on https:// and there are at most kOtaMaxHops of
// them; the download then starts at the final URL. Pure: the caller supplies
// `fetch`, which requests one URL (headers only) and returns the HTTP status,
// writing the Location header value as the server sent it to `next` for a 3xx,
// or a negative number when the server cannot be reached or the value does not
// fit in `cap` bytes.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

constexpr int kOtaMaxHops = 5;   // GitHub release assets take one

enum class OtaResolve { OK, INSECURE, TOO_MANY, FAILED };

inline bool ota_is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// `url` (a NUL-terminated absolute URL in a buffer of `cap` bytes) becomes the
// target of the Location value `loc`: an absolute URL replaces it, "//host/p"
// keeps its scheme, "/p?q" keeps its scheme and host. A path-relative reference
// is refused (the HTTP client cannot follow one either), and so is a value with
// a space or control character in it. The query stays.
inline bool ota_follow(char* url, const char* loc, size_t cap) {
    const size_t n = strnlen(loc, cap);
    if (n == 0 || n >= cap) return false;
    for (size_t i = 0; i < n; i++)
        if (static_cast<unsigned char>(loc[i]) <= ' ' || loc[i] == 0x7f) return false;
    size_t keep = 0;                                   // how much of `url` the target begins with
    if (loc[0] == '/') {
        const char* colon = std::strstr(url, "://");
        if (!colon) return false;
        keep = static_cast<size_t>(colon - url) + 1;   // "https:"
        if (loc[1] != '/') keep += 2 + std::strcspn(colon + 3, "/?#");   // "//host"
    } else {
        const char* p = loc;                           // "scheme://..."
        if (!std::isalpha(static_cast<unsigned char>(*p))) return false;
        while (std::isalnum(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-' || *p == '.') p++;
        if (std::strncmp(p, "://", 3) != 0) return false;
    }
    if (keep + n >= cap) return false;
    std::memcpy(url + keep, loc, n + 1);
    return true;
}

// `out` and `next` are two caller buffers of `cap` bytes (signed download
// URLs run to ~1 KB, too much for the update task's stack); `out` receives the
// URL to download from.
template <class Fetch>
OtaResolve ota_resolve_redirects(const char* url, char* out, char* next, size_t cap, Fetch&& fetch) {
    if (std::snprintf(out, cap, "%s", url) >= static_cast<int>(cap)) return OtaResolve::FAILED;
    for (int hop = 0;; hop++) {
        if (strncasecmp(out, "https://", 8) != 0) return OtaResolve::INSECURE;
        next[0] = '\0';
        const int status = fetch(out, next, cap);
        if (status < 0) return OtaResolve::FAILED;
        if (!ota_is_redirect(status)) return OtaResolve::OK;   // the server answers here
        if (hop == kOtaMaxHops) return OtaResolve::TOO_MANY;
        if (!ota_follow(out, next, cap)) return OtaResolve::FAILED;
    }
}
