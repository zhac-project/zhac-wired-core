// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// device.list helpers: page order and row encoding.
//
// Pages walk the pool in IEEE order and the cursor is the last IEEE sent, so a
// cursor stays valid when devices join or leave between two page requests. A
// pool-index cursor would skip a device whenever an earlier slot is freed, and
// the cloud's reconcile then deletes the skipped device from its cache as
// absent. Header-only and free of ESP-IDF so it runs in a host test.
#pragma once

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Slot of the live device with the smallest IEEE strictly greater than
// `after`, or -1 when none is left. `after` = 0 starts from the first device.
// ponytail: O(pool) per row, O(pool²) per full walk (200 slots ≈ 40k compares
// under the pool lock, microseconds); sort a slot index if the pool grows a lot.
template <typename IeeeOf, typename Live>
int devlist_next(size_t n, uint64_t after, IeeeOf ieee_of, Live live) {
    int best = -1;
    uint64_t best_ieee = 0;
    for (size_t i = 0; i < n; i++) {
        if (!live(i)) continue;
        const uint64_t v = ieee_of(i);
        if (v > after && (best < 0 || v < best_ieee)) {
            best = static_cast<int>(i);
            best_ieee = v;
        }
    }
    return best;
}

// JSON-escape `src` into `dst`, NUL-terminated, at most cap-1 bytes: `"` and
// `\` get a backslash, bytes below 0x20 become \u00XX, bytes from 0x80 up pass
// through so UTF-8 names stay readable. A string that does not fit is cut
// before the first character that would overflow — never inside an escape or
// a UTF-8 sequence — so the result is always valid between quotes. Returns
// the length written.
inline size_t json_escape(const char* src, char* dst, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char* s = reinterpret_cast<const unsigned char*>(src ? src : "");
    size_t n = 0;
    while (*s) {
        char esc[6];
        const char* piece = esc;
        size_t len, step;
        if (*s == '"' || *s == '\\') {
            esc[0] = '\\'; esc[1] = static_cast<char>(*s); len = 2; step = 1;
        } else if (*s < 0x20) {
            esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
            esc[4] = hex[*s >> 4]; esc[5] = hex[*s & 15]; len = 6; step = 1;
        } else {                                   // one whole UTF-8 sequence
            piece = reinterpret_cast<const char*>(s);
            len = 1;
            while ((s[len] & 0xC0) == 0x80) len++;
            step = len;
        }
        if (n + len >= cap) break;
        std::memcpy(dst + n, piece, len);
        n += len;
        s += step;
    }
    if (cap) dst[n] = '\0';
    return n;
}

// One device.list row. Strings are raw; devlist_row escapes them.
struct DevlistRow {
    uint64_t    ieee;
    unsigned    nwk;
    const char* friendly;
    const char* model;
    const char* manufacturer;
    const char* vendor;
    const char* model_id;
    bool        known;
    int64_t     last_seen;
    unsigned    lqi;
    int         battery;
    unsigned    ep_count;
};

// Escaped bytes per string field (NUL included). Plain names (≤ 33 B in the
// pool) always fit; only names full of quotes or control characters, which
// escaping grows up to 6×, get cut.
constexpr size_t kDevlistFieldCap = 64;
// A whole row with every field at its cap: ~208 B of keys and numbers plus
// 6 × 63 B of strings (friendly is sent twice, as "friendly" and "name").
constexpr size_t kDevlistRowCap = 640;

// Formats one row (`first` = no leading comma). Returns snprintf's length:
// <= 0 or >= cap means it did not fit.
inline int devlist_row(const DevlistRow& r, bool first, char* row, size_t cap) {
    char fr[kDevlistFieldCap], mo[kDevlistFieldCap], mf[kDevlistFieldCap];
    char ve[kDevlistFieldCap], mi[kDevlistFieldCap];
    json_escape(r.friendly, fr, sizeof(fr));
    json_escape(r.model, mo, sizeof(mo));
    json_escape(r.manufacturer, mf, sizeof(mf));
    json_escape(r.vendor, ve, sizeof(ve));
    json_escape(r.model_id, mi, sizeof(mi));
    return std::snprintf(row, cap,
        "%s{\"ieee\":\"0x%016" PRIX64 "\",\"nwk\":%u,"
        "\"friendly\":\"%s\",\"name\":\"%s\","
        "\"model\":\"%s\",\"manufacturer\":\"%s\","
        "\"vendor\":\"%s\",\"model_id\":\"%s\",\"known\":%s,"
        "\"last_seen\":%" PRId64 ",\"lqi\":%u,\"battery\":%d,"
        "\"ep_count\":%u}",
        first ? "" : ",", r.ieee, r.nwk, fr, fr, mo, mf, ve, mi,
        r.known ? "true" : "false", r.last_seen, r.lqi, r.battery, r.ep_count);
}
