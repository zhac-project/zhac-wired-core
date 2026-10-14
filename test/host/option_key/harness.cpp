// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Device-option NVS keys hold the whole 64-bit address (review F19): the old
// "%014llx" key lost the last hex digit of every 16-digit address, so two
// devices differing there shared one record and a restore made up a third.
#include "device_options_key.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    const uint64_t a = 0xAABBCCDDEEFF0011ull, b = 0xAABBCCDDEEFF0012ull, z = 0x00124B0001020304ull;
    char ka[16], kb[16], kz[16];
    opt_key(a, ka); opt_key(b, kb); opt_key(z, kz);
    expect("keys fit NVS (15 chars)", std::strlen(ka) <= 15 && std::strlen(kz) <= 15);
    expect("neighbouring addresses get different keys", std::strcmp(ka, kb) != 0);
    uint64_t back = 0;
    expect("a key gives back the exact address", opt_key_parse(ka, &back) && back == a);
    expect("  also with leading zero digits", opt_key_parse(kz, &back) && back == z);
    expect("an old hex key is not a new one", !opt_key_parse("aabbccddeeff001", &back));
    expect("  nor is junk", !opt_key_parse("ozz", &back) && !opt_key_parse("", &back));

    // Old keys: which known device each one belonged to.
    const uint64_t known[] = {a, b, z, 0x00A4C13800000001ull};
    uint64_t who = 0;
    expect("an old key two devices shared is ambiguous",
           opt_old_key_owner("aabbccddeeff001", known, 4, &who) == 2);
    const uint64_t one[] = {a};
    expect("  with one of them known it is that one",
           opt_old_key_owner("aabbccddeeff001", one, 1, &who) == 1 && who == a);
    expect("a 14-digit old key is the full address",
           opt_old_key_owner("124b0001020304", known, 4, &who) == 1 && who == z);
    const uint64_t f15[] = {0x0123456789ABCDEFull};
    expect("a 15-digit address was stored whole",
           opt_old_key_owner("123456789abcdef", f15, 1, &who) == 1 && who == f15[0]);
    expect("  another one", opt_old_key_owner("a4c13800000001", known, 4, &who) == 1 &&
                            who == 0x00A4C13800000001ull);
    expect("an old key of no known device has no owner",
           opt_old_key_owner("123456789012345", known, 4, &who) == 0);
    expect("a new key is not an old one", opt_old_key_owner(ka, known, 4, &who) == -1);

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\noption keys OK\n", g_fail);
    return g_fail;
}
