// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fakes for device_configure_request(): a two-device pool and the configure queue. See run.sh.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "zap_common.h"

static ZapDevice g_dev[2];
static int       g_dev_n  = 0;
static int       g_locked = 0;
void       zigbee_pool_lock() { g_locked++; }
void       zigbee_pool_unlock() { g_locked--; }
ZapDevice* pool_find_by_ieee(uint64_t ieee) {
    for (int i = 0; i < g_dev_n; i++)
        if (g_dev[i].ieee_addr == ieee) return &g_dev[i];
    return nullptr;
}

static uint64_t g_queued[4];
static int      g_queued_n = 0;
static int      g_locked_at_queue = 0;
void zigbee_configure_enqueue(uint64_t ieee) {
    g_locked_at_queue += g_locked;
    if (g_queued_n < 4) g_queued[g_queued_n++] = ieee;
}

#include "slice.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    const uint64_t done = 0xA4C1380000000001ull, blank = 0xA4C1380000000002ull, absent = 0xA4C1380000000003ull;
    g_dev[0] = ZapDevice{};
    g_dev[0].ieee_addr          = done;
    g_dev[0].configure_state    = static_cast<uint8_t>(ConfigureState::DONE);
    g_dev[0].configure_attempts = 9;   // gave up earlier
    std::strcpy(g_dev[0].model_id, "TS0201");
    g_dev[1] = ZapDevice{};
    g_dev[1].ieee_addr = blank;        // never interviewed: no model
    g_dev_n = 2;

    expect("unknown device: refused", !device_configure_request(absent));
    expect("never interviewed: refused", !device_configure_request(blank));
    expect("refused requests queue nothing", g_queued_n == 0);

    expect("configured device: accepted", device_configure_request(done));
    expect("queued once, for that device", g_queued_n == 1 && g_queued[0] == done);
    expect("starts fresh: PENDING, so the worker does not skip it as DONE",
           g_dev[0].configure_state == static_cast<uint8_t>(ConfigureState::PENDING));
    expect("starts fresh: its failed tries are forgotten", g_dev[0].configure_attempts == 0);
    expect("pool lock released, and not held while queueing", g_locked == 0 && g_locked_at_queue == 0);

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
