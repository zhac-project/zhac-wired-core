// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hub's own broadcasts (address lookups after a failed send, groupcasts
// for collections; research 2026-11-18 B1, B5) stay within 4 in any 9 s.
#include "../../esp_zb_bcast_budget.h"

#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    BroadcastBudget b;
    bool all = true;
    for (std::uint32_t t = 0; t < 4; t++) all = b.take(t) && all;
    expect("4 broadcasts at once: allowed", all);
    expect("a 5th within 9 s: refused", !b.take(100));
    expect("refused again a moment later", !b.take(8999));
    expect("9 s after the first: allowed again", b.take(9000));
    expect("  one only: the second is still within its 9 s", !b.take(9000));
    expect("9 s after the second: allowed", b.take(9001));
    {
        BroadcastBudget w;
        w.take(0xFFFFF000u);
        w.take(0xFFFFF001u);
        w.take(0xFFFFF002u);
        w.take(0xFFFFF003u);
        expect("across the millisecond counter's wrap: still refused", !w.take(0x00000010u));
        expect("  and allowed once 9 s have passed", w.take(0xFFFFF000u + BroadcastBudget::kWindowMs));
    }
    return g_fail;
}
