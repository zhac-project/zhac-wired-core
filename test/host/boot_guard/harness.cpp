// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The early-crash guard (ota_update.cpp) used to roll back on any 3 early crashes, before the radio and Lua
// guards had a chance (research 2026-11-18 A1). Safe mode comes first; only an image that keeps crashing in
// safe mode and has not yet run a day is rolled back.
#include "boot_guard_rule.h"

#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    using boot_guard::verdict;
    using V = boot_guard::Verdict;
    expect("0-2 early crashes: run normally", verdict(0, 0, true) == V::Run && verdict(2, 0, true) == V::Run);
    expect("3 early crashes: safe mode, not a rollback", verdict(3, 0, true) == V::SafeMode);
    expect("4 early crashes: still safe mode", verdict(4, 0, true) == V::SafeMode);
    expect("5 on a new image with a bootable other slot: rollback", verdict(5, 0, true) == V::Rollback);
    expect("  also at 23 h of run time", verdict(7, 23, true) == V::Rollback);
    expect("5 on an image that ran a day: safe mode", verdict(5, 24, true) == V::SafeMode);
    expect("5 with no other image to boot: safe mode", verdict(5, 0, false) == V::SafeMode);

    using boot_guard::image_hours;
    expect("the running image's own count carries over", image_hours(0xAABBCCDD, 7, 0xAABBCCDD) == 7);
    expect("another image's count does not", image_hours(0x11223344, 24, 0xAABBCCDD) == 0);
    expect("the count stops at a day", boot_guard::next_hour(24) == 24 && boot_guard::next_hour(5) == 6);
    return g_fail;
}
