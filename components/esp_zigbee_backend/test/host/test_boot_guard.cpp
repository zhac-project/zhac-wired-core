// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The radio is skipped only after it crashed the hub several boots in a row,
// a radio-less hub tries again by itself, and a crash after the network came
// up is not the radio's (review 2026-10-24 ZR-05 / PB-03).
#include "../../esp_zb_boot_guard.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

using R = BootGuard::Reset;

// One boot that starts the radio and crashes before the network is up.
static bool crash_starting(BootGuard& g) {
    g.arm();
    return g.on_boot(R::CRASH);
}

int main() {
    BootGuard g;
    std::memset(&g, 0xA5, sizeof(g));   // no-init RAM after power-on
    expect("power-on: garbage is cleared, the radio starts", !g.on_boot(R::OTHER));
    std::memset(&g, 0xA5, sizeof(g));
    expect("a crash with garbage in no-init RAM counts nothing", !g.on_boot(R::CRASH));

    expect("one crash while starting the radio: try again", !crash_starting(g));
    expect("two crashes in a row: try again", !crash_starting(g));
    expect("three in a row: the next boot runs without the radio", crash_starting(g));
    expect("  first retry in 10 min", g.next_retry_s() == 600);

    g.retrying();
    expect("the retry restart starts the radio", !g.on_boot(R::RESTART));
    expect("  one more crash while starting ends it again", crash_starting(g));
    expect("  next retry in 1 h", g.next_retry_s() == 3600);
    g.retrying();
    g.on_boot(R::RESTART);
    crash_starting(g);
    expect("  then every 6 h", g.next_retry_s() == 6 * 3600);
    g.retrying();
    g.on_boot(R::RESTART);
    crash_starting(g);
    expect("  and stays at 6 h", g.next_retry_s() == 6 * 3600);

    expect("a restart by hand also tries the radio once", !g.on_boot(R::RESTART));
    g.arm();
    g.network_up();
    expect("the network comes up: the counts start over",
           !g.on_boot(R::CRASH) && g.next_retry_s() == 600);

    crash_starting(g);
    crash_starting(g);
    g.arm();
    g.network_up();
    expect("a crash after the network is up is not the radio's", !g.on_boot(R::CRASH));
    expect("  nor two more while starting", !crash_starting(g) && !crash_starting(g));

    g.arm();
    g.disarm();                          // esp_zigbee_init failed without a crash
    expect("a crash after a graceful radio failure is not counted", !g.on_boot(R::CRASH) &&
           !crash_starting(g) && !crash_starting(g));
    expect("power cycle: everything starts over", !g.on_boot(R::OTHER) && !crash_starting(g));

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nboot guard OK\n", g_fail);
    return g_fail;
}
