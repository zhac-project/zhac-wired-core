// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// LQI derived from RSSI (esp_zb_lqi.h). The radio's own LQI byte sits at 9..11
// whatever the signal, so this mapping is the only link-quality number the UI
// and the cloud get from this backend.

#include "../../esp_zb_lqi.h"

#include <cstdio>

static int g_fail = 0;

static void expect_eq(const char* what, long got, long want) {
    if (got != want) {
        g_fail++;
        std::printf("FAIL %s: got %ld want %ld\n", what, got, want);
    } else {
        std::printf("ok   %s == %ld\n", what, want);
    }
}

int main() {
    using zhc_zb::lqi_from_rssi;

    expect_eq("-110 dBm, below sensitivity", lqi_from_rssi(-110), 0);
    expect_eq("-100 dBm, window floor",      lqi_from_rssi(-100), 0);
    expect_eq("-94 dBm, weak link",          lqi_from_rssi(-94), 23);
    expect_eq("-68 dBm, mid window",         lqi_from_rssi(-68), 127);
    expect_eq("-47 dBm, close link",         lqi_from_rssi(-47), 211);
    expect_eq("-36 dBm, window top",         lqi_from_rssi(-36), 255);
    expect_eq("-20 dBm, saturated",          lqi_from_rssi(-20), 255);

    // A stronger signal never reads as a worse link, over the whole int8 range.
    for (int r = -128; r < 127; ++r) {
        if (lqi_from_rssi(static_cast<int8_t>(r)) > lqi_from_rssi(static_cast<int8_t>(r + 1))) {
            g_fail++;
            std::printf("FAIL not monotonic at %d dBm\n", r);
        }
    }

    if (g_fail) {
        std::printf("\n%d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("\nLQI mapping OK\n");
    return 0;
}
