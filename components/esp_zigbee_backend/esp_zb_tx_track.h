// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_tx_track -- the raw APS sends still waiting for the stack's confirm.
//
// Every frame the hub sends with ezb_apsde_data_request (commands, reads,
// pings, replies) was fire-and-forget: a device that had moved to another
// short address, or lost its route, missed the command and the page said OK
// (research 2026-11-18 B1). The stack confirms each such request (the APS ack,
// or the failure after its own retries); this table keeps a copy of the frame
// until then, so the first failure can be retried once after the address is
// looked up again, as zigbee-herdsman does. The second failure is final.
//
// Matching: the confirm names the destination and cluster and carries a copy
// of the sent bytes (esp-zigbee-lib aps_apsde_user_data_confirm); only a frame
// with the same bytes is ours. A confirm without bytes (the stack could not
// copy them) takes the oldest frame to that destination and cluster.
//
// Pure C++ (no ESP-IDF, no SDK) so the policy has a host test; the backend
// adds the lock, the confirm handler, the address lookup and the resend.
// ponytail: 16 slots, the oldest frame goes when they are all taken (its
// confirm is then simply not ours); size it up if confirms go missing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tx_track {

constexpr std::size_t   kSlots    = 16;
constexpr std::size_t   kFrameMax = 128;            // esp_zb_adapter_send's cap
constexpr std::uint32_t kStaleMs  = 60u * 1000u;    // no confirm or lookup by then: forget it

struct Frame {
    std::uint16_t nwk;
    std::uint8_t  ep;
    std::uint16_t cluster;
    std::uint8_t  len;
    std::uint8_t  data[kFrameMax];
};

enum class Verdict : std::uint8_t {
    kNone,     // delivered, or a frame this table does not hold
    kRetry,    // first failure: look the address up, then resend
    kFailed,   // the resend failed too
};

class Table {
public:
    // A frame is about to go to the stack; `resend` = this is its one retry.
    // Returns its token, or 0 when it is not tracked (empty or too long).
    std::uint32_t sent(const Frame& f, bool resend, std::uint32_t now_ms) {
        if (f.len == 0 || f.len > kFrameMax) return 0;
        Slot& s = victim(now_ms);
        if (++next_ == 0) ++next_;
        s = {};
        s.token  = next_;
        s.t_ms   = now_ms;
        s.resend = resend;
        s.f      = f;
        return s.token;
    }

    // The stack refused the request: there will be no confirm.
    void cancel(std::uint32_t token) {
        if (Slot* s = by_token(token)) *s = {};
    }

    // The stack's confirm. kRetry: the frame waits for its address lookup,
    // `*token` names it (looking_up, then take). kNone and kFailed: the frame
    // left the table.
    Verdict confirm(std::uint16_t nwk, std::uint16_t cluster, const std::uint8_t* asdu,
                    std::uint16_t asdu_len, std::uint8_t status, std::uint32_t* token) {
        const bool bytes = asdu && asdu_len;
        Slot* hit = nullptr;
        for (Slot& s : slots_) {
            if (!s.token || s.lookup || s.f.nwk != nwk || s.f.cluster != cluster) continue;
            if (!bytes) {
                if (!hit || older(s, *hit)) hit = &s;
            } else if (s.f.len == asdu_len && std::memcmp(s.f.data, asdu, s.f.len) == 0) {
                hit = &s;
                break;
            }
        }
        if (!hit) return Verdict::kNone;
        if (status == 0) { *hit = {}; return Verdict::kNone; }
        if (hit->resend) { *hit = {}; return Verdict::kFailed; }
        hit->lookup = true;
        if (token) *token = hit->token;
        return Verdict::kRetry;
    }

    // The lookup for `token` went out, for `ieee`.
    void looking_up(std::uint32_t token, std::uint64_t ieee) {
        if (Slot* s = by_token(token)) s->ieee = ieee;
    }

    // The lookup ended: the frame and its device leave the table. False when
    // the token is gone (stale, or taken already).
    bool take(std::uint32_t token, Frame* out, std::uint64_t* ieee) {
        Slot* s = by_token(token);
        if (!s || !s->lookup) return false;
        if (out) *out = s->f;
        if (ieee) *ieee = s->ieee;
        *s = {};
        return true;
    }

private:
    struct Slot {
        std::uint32_t token;    // 0 = free
        std::uint32_t t_ms;     // sent at
        std::uint64_t ieee;     // the device a lookup is for
        bool          resend;
        bool          lookup;   // failed once, waiting for the address lookup
        Frame         f;
    };

    // Sent before `b` (wrap-safe within half the 32-bit range).
    bool older(const Slot& a, const Slot& b) const {
        return static_cast<std::int32_t>(a.t_ms - b.t_ms) < 0;
    }

    Slot* by_token(std::uint32_t token) {
        if (token == 0) return nullptr;
        for (Slot& s : slots_) if (s.token == token) return &s;
        return nullptr;
    }

    // A free slot; else a stale one; else the oldest not being looked up;
    // else the oldest.
    Slot& victim(std::uint32_t now_ms) {
        Slot* oldest = nullptr;
        Slot* oldest_sent = nullptr;
        for (Slot& s : slots_) {
            if (!s.token || now_ms - s.t_ms >= kStaleMs) return s;
            if (!oldest || older(s, *oldest)) oldest = &s;
            if (!s.lookup && (!oldest_sent || older(s, *oldest_sent))) oldest_sent = &s;
        }
        return oldest_sent ? *oldest_sent : *oldest;
    }

    Slot          slots_[kSlots] = {};
    std::uint32_t next_          = 0;
};

}  // namespace tx_track
