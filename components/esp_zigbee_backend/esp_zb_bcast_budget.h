// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_bcast_budget -- how many broadcasts the hub may start on its own.
//
// Every router keeps each broadcast it relays in its broadcast transaction
// table for about 9 s (nwkBroadcastDeliveryTime), and the table is small
// (the spec's minimum is 9 entries). Broadcasts past that are dropped, and
// they include the network's own route requests. The hub's extra ones (an
// address lookup after a failed send, a groupcast for a collection;
// research 2026-11-18 B1, B5) stay within 4 in any 9 s; a caller refused
// here falls back to what it did before (no lookup; unicasts).
// Not thread-safe: the caller locks.
#pragma once

#include <cstdint>

class BroadcastBudget {
public:
    static constexpr int           kMax      = 4;
    static constexpr std::uint32_t kWindowMs = 9000;

    bool take(std::uint32_t now_ms) {
        if (n_ < kMax) {
            t_[n_++] = now_ms;
            return true;
        }
        // The oldest of the last kMax broadcasts, ring order.
        if (now_ms - t_[next_] < kWindowMs) return false;
        t_[next_] = now_ms;
        next_ = (next_ + 1) % kMax;
        return true;
    }

private:
    std::uint32_t t_[kMax] = {};
    int           n_       = 0;
    int           next_    = 0;
};
