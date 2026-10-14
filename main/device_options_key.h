// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// NVS keys for per-device options. An NVS key holds 15 characters, so the
// old "%014llx" key (16 hex digits into a 16-byte buffer) lost the last digit
// of every 16-digit address: neighbouring devices shared one record, and a
// restore parsed the 15 digits into a made-up address (review F19). The key
// is now 'o' + the address in 13 base-32 digits (0-9a-v), which keeps all 64
// bits; old keys are moved over once at boot (device_options_restore_all).
// Pure; host check test/host/option_key/.
#pragma once

#include <cstdint>
#include <cstring>

inline void opt_key(uint64_t ieee, char out[16]) {
    static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuv";
    out[0] = 'o';
    for (int i = 13; i >= 1; i--) { out[i] = kDigits[ieee & 31]; ieee >>= 5; }
    out[14] = '\0';
}

inline bool opt_key_parse(const char* key, uint64_t* ieee) {
    if (!key || key[0] != 'o' || std::strlen(key) != 14) return false;
    uint64_t v = 0;
    for (int i = 1; i <= 13; i++) {
        const char c = key[i];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'v') d = c - 'a' + 10;
        else return false;
        if (i == 1 && d > 15) return false;   // 13 digits x 5 bits = 65: the first holds 4
        v = (v << 5) | static_cast<uint64_t>(d);
    }
    *ieee = v;
    return true;
}

// Which of `known` an old hex key belonged to: the number of candidates (the
// owner in *who when exactly one), or -1 when `key` is not an old key. A
// 15-digit old key is either a whole 15-digit address or the first 15 digits
// of a 16-digit one.
inline int opt_old_key_owner(const char* key, const uint64_t* known, int n, uint64_t* who) {
    const size_t len = key ? std::strlen(key) : 0;
    if (len < 1 || len > 15) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = key[i];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return -1;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    int found = 0;
    for (int i = 0; i < n; i++) {
        const bool whole     = known[i] == v;
        const bool truncated = len == 15 && known[i] >= (1ull << 60) && (known[i] >> 4) == v;
        if (whole || truncated) { if (!found) *who = known[i]; found++; }
    }
    return found;
}
