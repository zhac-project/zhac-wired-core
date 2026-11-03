// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_boot_guard -- when a boot runs without the radio, and when it tries
// the radio again.
//
// With no radio firmware on the C6 (a new Guition board ships ESP-Hosted, not
// ot_rcp) or a mismatched one, the RCP handshake ends in abort(): unguarded,
// a boot loop with no web UI to say why. The guard is armed while the radio
// starts and disarmed when the stack reports a network, so a crash later (a
// decode bug, an unrelated task) is not the radio's. Only kTrips crashes in a
// row while armed make a boot skip the radio (review 2026-10-24 ZR-05 /
// PB-03: one crash used to leave the hub without Zigbee until someone reset
// it), and such a boot restarts by itself to try again: after 10 min, 1 h,
// then every 6 h. Any restart tries the radio once more.
//
// Kept in no-init RAM: it survives a panic, a watchdog reset and esp_restart,
// not a power cycle (`magic` tells garbage apart).
#pragma once

#include <cstdint>

struct BootGuard {
    uint32_t magic;
    uint8_t  armed;     // the radio is starting, no network yet
    uint8_t  crashes;   // boots in a row that crashed while armed
    uint8_t  retries;   // restarts that tried the radio again (the backoff)
    uint8_t  _pad;

    static constexpr uint32_t kMagic = 0x5A424755;   // "ZBGU"
    static constexpr uint8_t  kTrips = 3;

    enum class Reset : uint8_t { CRASH, RESTART, OTHER };   // panic / watchdog; esp_restart; the rest

    // At boot, before the radio. True = run this boot without it.
    bool on_boot(Reset r) {
        if (r == Reset::OTHER || magic != kMagic) {
            *this = BootGuard{kMagic, 0, 0, 0, 0};
        } else if (r == Reset::CRASH) {
            crashes = armed ? static_cast<uint8_t>(crashes < kTrips ? crashes + 1 : kTrips) : 0;
        } else if (crashes >= kTrips) {
            crashes = kTrips - 1;   // a restart tries the radio once more
        }
        armed = 0;
        return crashes >= kTrips;
    }
    void arm()    { armed = 1; }
    void disarm() { armed = 0; }   // the radio failed without a crash
    void network_up() { armed = 0; crashes = 0; retries = 0; }

    // A boot without the radio: seconds until it restarts to try again.
    uint32_t next_retry_s() const {
        static constexpr uint32_t kDelayS[] = {600, 3600, 6 * 3600};
        return kDelayS[retries < 2 ? retries : 2];
    }
    void retrying() { if (retries < 255) retries++; }   // just before that restart
};
