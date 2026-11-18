// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_configure_kick -- when a frame from a device queues its configure again.
//
// A frame from a device whose configure never completed means it is awake
// right now -- the only moment a bind / attribute write reaches a sleepy
// end-device (the cube's binds failed on every timed retry). Such a frame
// re-queues configure while it is listening; the queue skips devices already
// DONE. Rate-limited per device so a chatty sensor does not queue it per
// report.
//
// Capped like z2m, which tries a device's configure 3 times per run, on its
// messages: past the configure queue's 5 timed tries a device gets 3 more
// here, then stays FAILED until it pairs again (leave and join) or the owner
// presses Configure -- both start its count again. A device that refuses a
// mandatory report fails every run (research 2026-11-18 A5); uncapped, it was
// configured again every 30 s for good, and two of them talking in turn
// defeated a single-slot rate limit (P1 review 2026-11-19 #1). Not
// thread-safe: the stack task only.
#pragma once

#include <cstdint>

class ConfigureKick {
public:
    static constexpr uint8_t kMaxAttempts = 5 + 3;           // the queue's MAX_ATTEMPTS, then 3 while awake
    static constexpr int64_t kEveryUs     = 30 * 1000000LL;  // per device
    static constexpr int     kSlots       = 16;              // devices rate-limited at once; the 17th evicts one

    // matched: SupportState::MATCHED; configured: ConfigureState::DONE;
    // attempts: ZapDevice::configure_attempts.
    bool due(uint64_t ieee, bool matched, bool configured, uint8_t attempts, int64_t now_us) {
        if (!matched || configured || attempts >= kMaxAttempts) return false;
        Slot* slot = nullptr;
        for (auto& s : slots_) if (s.ieee == ieee) slot = &s;
        if (slot && now_us - slot->us <= kEveryUs) return false;
        if (!slot) {
            slot  = &slots_[next_];
            next_ = (next_ + 1) % kSlots;
        }
        *slot = {ieee, now_us};
        return true;
    }

private:
    struct Slot { uint64_t ieee; int64_t us; };
    Slot slots_[kSlots] = {};
    int  next_ = 0;
};
