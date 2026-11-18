// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A frame from an awake, not-yet-configured device queues its configure again:
// once per 30 s per device, and only while it has tries left (P1 review
// 2026-11-19 #1: uncapped, a device that refuses a mandatory report was
// configured again every 30 s for good).
#include "../../esp_zb_configure_kick.h"

#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static constexpr int64_t S = 1000000LL;   // one second in us

int main() {
    const uint64_t a = 0xA4C1380000000001ull, b = 0xA4C1380000000002ull;
    {
        ConfigureKick k;
        expect("awake, matched, not configured: configure queued", k.due(a, true, false, 0, 10 * S));
        expect("again within 30 s: not queued", !k.due(a, true, false, 0, 20 * S));
        expect("after 30 s: queued again", k.due(a, true, false, 0, 41 * S));
        expect("configured (DONE): never queued", !k.due(b, true, true, 0, 50 * S));
        expect("no definition (not MATCHED): never queued", !k.due(b, false, false, 0, 51 * S));
    }
    {
        ConfigureKick k;
        expect("7 failed tries (5 timed + 2 while awake): one more", k.due(a, true, false, 7, 10 * S));
        expect("8 failed tries: no more until it pairs again or the owner presses Configure",
               !k.due(b, true, false, 8, 10 * S));
        expect("the counter at its 255 ceiling: still none", !k.due(b, true, false, 255, 100 * S));
    }
    {
        // Two failing devices talking in turn defeated the single-slot limit:
        // every frame queued a full configure.
        ConfigureKick k;
        expect("a: queued", k.due(a, true, false, 5, 10 * S));
        expect("b: queued", k.due(b, true, false, 5, 11 * S));
        expect("a again 2 s later: not queued", !k.due(a, true, false, 5, 12 * S));
        expect("b again 2 s later: not queued", !k.due(b, true, false, 5, 13 * S));
        expect("a after its 30 s: queued", k.due(a, true, false, 5, 41 * S));
    }
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
