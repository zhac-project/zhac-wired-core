// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Follow an update URL's redirects before the download starts.
//
// esp_https_ota follows redirects itself, but on IDF 6.0 it takes one to
// http:// (a certificate bundle does not stop that) and its connect loop never
// checks max_redirection_count, so a redirect loop held the updater forever
// (review F09, F33). Here every hop must stay on https:// and there are at most
// kOtaMaxHops of them; the download then starts at the final URL. Pure: the
// caller supplies `fetch`, which requests one URL (headers only) and returns
// the HTTP status, writing the absolute redirect target to `next` for a 3xx,
// or a negative number when the server cannot be reached.
#pragma once

#include <cstdio>
#include <cstring>
#include <strings.h>

constexpr int kOtaMaxHops = 5;   // GitHub release assets take one

enum class OtaResolve { OK, INSECURE, TOO_MANY, FAILED };

inline bool ota_is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
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
        const size_t n = strnlen(next, cap);
        if (n == 0 || n >= cap) return OtaResolve::FAILED;
        std::memcpy(out, next, n + 1);
    }
}
