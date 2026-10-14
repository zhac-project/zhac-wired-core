// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_forget_log -- which queued interviews a delete cancelled.
//
// Deleting a device removed its record but left its queued interview requests;
// the worker then upserted the address and recreated (and persisted) the
// deleted device without any new announce (review F15). Every queued request
// carries the queue counter it got; a delete records the counter at that
// moment, and requests for that device numbered up to it are dropped. A new
// join after the delete is numbered higher and goes ahead. A few recent
// deletes are enough: the queue holds seconds to minutes of work.
#pragma once

#include <cstdint>

class ForgetLog {
public:
    static constexpr int kSize = 8;

    void forget(uint64_t ieee, uint32_t seq) {
        for (auto& e : log_) if (e.ieee == ieee) { e.seq = seq; return; }
        log_[next_] = {ieee, seq};
        next_ = (next_ + 1) % kSize;
    }
    bool cancelled(uint64_t ieee, uint32_t seq) const {
        for (const auto& e : log_) if (e.ieee == ieee && ieee != 0) return seq <= e.seq;
        return false;
    }

private:
    struct Entry { uint64_t ieee; uint32_t seq; };
    Entry log_[kSize] = {};
    int   next_ = 0;
};
