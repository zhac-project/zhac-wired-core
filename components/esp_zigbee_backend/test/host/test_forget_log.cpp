// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Deleting a device cancels the interviews already queued for it, but not a
// genuinely new join afterwards (review F15).
#include "../../esp_zb_forget_log.h"

#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    ForgetLog log;
    const uint64_t a = 0xA4C1380000000001ull, b = 0xA4C1380000000002ull;
    expect("nothing is cancelled at first", !log.cancelled(a, 1));

    // a queued at 5 (reinterview), then deleted when the queue counter was 7
    log.forget(a, 7);
    expect("the queued interview is cancelled", log.cancelled(a, 5));
    expect("  also one queued just before the delete", log.cancelled(a, 7));
    expect("a new join after the delete is not", !log.cancelled(a, 8));
    expect("another device is not", !log.cancelled(b, 5));

    for (uint64_t i = 0; i < ForgetLog::kSize + 3; i++) log.forget(0x1000 + i, 100 + i);
    expect("the newest deletes are kept", log.cancelled(0x1000 + ForgetLog::kSize + 2, 50));
    expect("  the oldest roll off", !log.cancelled(a, 5));

    log.forget(b, 10);
    log.forget(b, 20);                          // deleted again later
    expect("a second delete moves the line", log.cancelled(b, 15) && !log.cancelled(b, 21));

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nforget log OK\n", g_fail);
    return g_fail;
}
