// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The setup access point (review 2026-11-09 WN-01, WN-02): a fresh hub opens it
// 30 s without a network; a set-up hub (stored Wi-Fi, or Ethernet seen once)
// only after 15 min without one, so a router that boots slower than the hub
// opens nothing. `ap_disabled` keeps it shut and closes it. It closes once a
// network has held 2 min, or after 30 min in any case; once a network has
// held 2 min in a boot, it stays shut until the next boot.
#include <cstdint>
#include <cstdio>

#include "slice.inc"   // kAp*, ap_step, cut from components/wifi_hosted/wifi_hosted.cpp

static int g_fail = 0;
static void expect(const char* what, int got, int want) {
    std::printf("%s %-58s -> %+d\n", got == want ? "ok  " : "FAIL", what, got);
    if (got != want) g_fail++;
}

int main() {
    constexpr uint32_t s = 1000, min = 60 * s;
    // Closed AP: fresh hub (set_up = false)
    expect("fresh, 29 s down", ap_step(false, false, false, false, 29 * s, 0, 0), 0);
    expect("fresh, 30 s down", ap_step(false, false, false, false, 30 * s, 0, 0), 1);
    expect("fresh, network up", ap_step(false, false, false, false, 0, 5 * s, 0), 0);
    expect("fresh, ap_disabled, 1 h down", ap_step(false, true, false, false, 60 * min, 0, 0), 0);
    // Closed AP: set-up hub -- the power-cut case of WN-01
    expect("set up, 30 s down (router still booting)", ap_step(false, false, true, false, 30 * s, 0, 0), 0);
    expect("set up, 10 min down (set-up window)", ap_step(false, false, true, false, 10 * min, 0, 0), 0);
    expect("set up, 14:59 down", ap_step(false, false, true, false, 15 * min - 1, 0, 0), 0);
    expect("set up, 15 min down (way back in)", ap_step(false, false, true, false, 15 * min, 0, 0), 1);
    expect("set up, ap_disabled, 1 h down", ap_step(false, true, true, false, 60 * min, 0, 0), 0);
    // Open AP
    expect("open, no network, 1 min", ap_step(true, false, false, false, 2 * min, 0, 1 * min), 0);
    expect("open, network 119 s", ap_step(true, false, true, false, 0, 119 * s, 5 * min), 0);
    expect("open, network 120 s", ap_step(true, false, true, false, 0, 120 * s, 5 * min), -1);
    expect("open 29:59, unused", ap_step(true, false, false, false, 30 * min, 0, 30 * min - 1), 0);
    expect("open 30 min, unused", ap_step(true, false, false, false, 31 * min, 0, 30 * min), -1);
    expect("open, ap_disabled set meanwhile", ap_step(true, true, false, false, 1 * min, 0, 10 * s), -1);
    // Settled: a network held 2 min this boot -- a later loss opens nothing (final review I2:
    // a jammed Wi-Fi or a router outage must not open the AP mid-uptime)
    expect("set up, settled, 1 h down", ap_step(false, false, true, true, 60 * min, 0, 0), 0);
    expect("fresh, settled, 1 h down", ap_step(false, false, false, true, 60 * min, 0, 0), 0);
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nap policy OK\n", g_fail);
    return g_fail;
}
