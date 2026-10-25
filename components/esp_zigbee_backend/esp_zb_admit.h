// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_admit -- which unknown devices may join the device pool.
//
// A device announcement is an unauthenticated broadcast that any network
// member can send for any address, and every unknown IEEE in one used to
// become a pool entry with a full interview and a flash record (review
// 2026-10-24 ZR-02). A new IEEE is now admitted, from an announce or from a
// frame the stack's address map resolves, only when the trust centre let it in
// a moment ago (DEVICE_AUTHORIZED, or a DEVICE_UPDATE it accepted), or while
// permit-join is open and for a grace period after it closes; and no more than
// kCap new ones per window. Devices already in the pool are never asked, so a
// known device rejoining is never refused. Not thread-safe: the caller locks.
#pragma once

#include <cstdint>

class AdmitGate {
public:
    static constexpr int     kAuthSlots = 16;
    static constexpr int64_t kAuthTtlUs = 10 * 60 * 1000000LL;   // authorised -> its announce / first frame
    static constexpr int64_t kGraceUs   = 5 * 60 * 1000000LL;    // after permit-join closes
    static constexpr int     kCap       = 32;                    // new devices per window
    static constexpr int64_t kWindowUs  = 10 * 60 * 1000000LL;

    enum class Verdict : uint8_t { ADMIT, NOT_AUTHORISED, CAPPED };

    void authorised(uint64_t ieee, int64_t now_us) {
        if (ieee == 0) return;
        Auth* slot = &auth_[next_];
        for (auto& a : auth_) if (a.ieee == ieee) slot = &a;
        if (slot == &auth_[next_]) next_ = (next_ + 1) % kAuthSlots;
        *slot = {ieee, now_us};
    }

    // Opened for `seconds`, or closed now (0).
    void permit_join(uint32_t seconds, int64_t now_us) {
        if (seconds == 0) {
            if (open_until_ > now_us) open_until_ = now_us;
            return;
        }
        const int64_t until = now_us + static_cast<int64_t>(seconds) * 1000000LL;
        if (until > open_until_) open_until_ = until;
    }

    Verdict admit(uint64_t ieee, int64_t now_us) {
        bool trusted = now_us <= open_until_ + kGraceUs;
        for (const auto& a : auth_)
            if (a.ieee == ieee && ieee != 0 && now_us - a.us <= kAuthTtlUs) trusted = true;
        if (!trusted) return Verdict::NOT_AUTHORISED;
        if (count_ == 0 || now_us - window_us_ >= kWindowUs) {
            window_us_ = now_us;
            count_     = 0;
        }
        if (count_ >= kCap) return Verdict::CAPPED;
        count_++;
        return Verdict::ADMIT;
    }

private:
    struct Auth { uint64_t ieee; int64_t us; };
    Auth    auth_[kAuthSlots] = {};
    int     next_       = 0;
    int64_t open_until_ = -kGraceUs - 1;   // never opened
    int64_t window_us_  = 0;
    int     count_      = 0;
};
