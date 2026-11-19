// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Raw APS sends waiting for their confirm (research 2026-11-18 B1): a frame
// the stack could not deliver is handed back once, for an address lookup and
// one resend; the resend failing is the end of it.
#include "../../esp_zb_tx_track.h"

#include <cstdio>
#include <cstring>

using namespace tx_track;

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

// genOnOff On, cluster-specific, with `tsn`.
static Frame on_cmd(std::uint16_t nwk, std::uint8_t tsn) {
    Frame f{};
    f.nwk = nwk; f.ep = 1; f.cluster = 0x0006; f.len = 3;
    f.data[0] = 0x01; f.data[1] = tsn; f.data[2] = 0x01;
    return f;
}

constexpr std::uint8_t kOk = 0x00, kNoAck = 0xA7;
constexpr std::uint64_t kIeee = 0xA4C1380000000001ull;

int main() {
    {
        Table t;
        const Frame f = on_cmd(0x1234, 7);
        expect("a frame that fits is tracked (token)", t.sent(f, false, 0) != 0);
        std::uint32_t tok = 0;
        expect("delivered: nothing to do", t.confirm(0x1234, 0x0006, f.data, f.len, kOk, &tok) == Verdict::kNone);
        expect("  and it left the table: a later failure for it is not ours",
               t.confirm(0x1234, 0x0006, f.data, f.len, kNoAck, &tok) == Verdict::kNone);
    }
    {
        Table t;
        const Frame f = on_cmd(0x1234, 7);
        t.sent(f, false, 0);
        std::uint32_t tok = 0;
        expect("first failure: retry", t.confirm(0x1234, 0x0006, f.data, f.len, kNoAck, &tok) == Verdict::kRetry);
        expect("  with a token", tok != 0);
        t.looking_up(tok, kIeee);
        Frame back{};
        std::uint64_t ieee = 0;
        expect("the lookup ends: the frame comes back", t.take(tok, &back, &ieee));
        expect("  byte for byte", back.len == f.len && std::memcmp(back.data, f.data, f.len) == 0 &&
                                   back.nwk == 0x1234 && back.ep == 1 && back.cluster == 0x0006);
        expect("  with the device it was looked up for", ieee == kIeee);
        expect("  once", !t.take(tok, &back, &ieee));
        back.nwk = 0x5678;   // the device moved
        t.sent(back, true, 100);
        expect("the resend failing too: failed",
               t.confirm(0x5678, 0x0006, back.data, back.len, kNoAck, &tok) == Verdict::kFailed);
        expect("  and it left the table", t.confirm(0x5678, 0x0006, back.data, back.len, kNoAck, &tok) == Verdict::kNone);
    }
    {
        Table t;
        const Frame a = on_cmd(0x1234, 7), b = on_cmd(0x1234, 8);
        t.sent(a, false, 0);
        t.sent(b, false, 1);
        std::uint32_t tok = 0;
        expect("two frames to one device: the confirm names the second",
               t.confirm(0x1234, 0x0006, b.data, b.len, kNoAck, &tok) == Verdict::kRetry);
        Frame back{};
        std::uint64_t ieee = 0;
        expect("  and the second comes back", t.take(tok, &back, &ieee) && back.data[1] == 8);
        expect("  the first still waits for its own",
               t.confirm(0x1234, 0x0006, a.data, a.len, kOk, &tok) == Verdict::kNone &&
               t.confirm(0x1234, 0x0006, a.data, a.len, kNoAck, &tok) == Verdict::kNone);
    }
    {
        Table t;
        const Frame a = on_cmd(0x1234, 7), b = on_cmd(0x1234, 8);
        t.sent(a, false, 0);
        t.sent(b, false, 1);
        std::uint32_t tok = 0;
        Frame back{};
        std::uint64_t ieee = 0;
        expect("a confirm without the bytes: the oldest to that device and cluster",
               t.confirm(0x1234, 0x0006, nullptr, 0, kNoAck, &tok) == Verdict::kRetry &&
               t.take(tok, &back, &ieee) && back.data[1] == 7);
    }
    {
        Table t;
        const Frame f = on_cmd(0x1234, 7);
        t.sent(f, false, 0);
        std::uint32_t tok = 0;
        expect("another device: not ours", t.confirm(0x9999, 0x0006, f.data, f.len, kNoAck, &tok) == Verdict::kNone);
        expect("another cluster: not ours", t.confirm(0x1234, 0x0008, f.data, f.len, kNoAck, &tok) == Verdict::kNone);
    }
    {
        Table t;
        const std::uint32_t tok = t.sent(on_cmd(0x1234, 7), false, 0);
        t.cancel(tok);   // the stack refused the request
        const Frame f = on_cmd(0x1234, 7);
        std::uint32_t got = 0;
        expect("cancelled: forgotten", t.confirm(0x1234, 0x0006, f.data, f.len, kNoAck, &got) == Verdict::kNone);
    }
    {
        Table t;
        Frame f = on_cmd(0x1234, 7);
        f.len = 0;
        expect("an empty frame is not tracked", t.sent(f, false, 0) == 0);
        f.len = kFrameMax + 1;
        expect("nor one longer than the slot", t.sent(f, false, 0) == 0);
    }
    {
        Table t;
        for (std::uint8_t i = 0; i <= kSlots; i++) t.sent(on_cmd(0x1000 + i, i), false, i);
        std::uint32_t tok = 0;
        const Frame first = on_cmd(0x1000, 0), last = on_cmd(0x1000 + kSlots, kSlots);
        expect("a full table forgets the oldest",
               t.confirm(first.nwk, 0x0006, first.data, first.len, kNoAck, &tok) == Verdict::kNone);
        expect("  and keeps the newest",
               t.confirm(last.nwk, 0x0006, last.data, last.len, kNoAck, &tok) == Verdict::kRetry);
    }
    {
        Table t;
        const Frame f = on_cmd(0x1234, 7);
        t.sent(f, false, 0);
        std::uint32_t tok = 0;
        t.confirm(0x1234, 0x0006, f.data, f.len, kNoAck, &tok);
        t.looking_up(tok, kIeee);
        // The lookup never ends (no callback); the slots fill up long after.
        for (std::uint8_t i = 0; i < kSlots; i++) t.sent(on_cmd(0x2000 + i, i), false, kStaleMs + i);
        Frame back{};
        std::uint64_t ieee = 0;
        expect("a lookup that never ended is forgotten once stale", !t.take(tok, &back, &ieee));
    }
    {
        Table t;
        const Frame f = on_cmd(0x1234, 7);
        t.sent(f, false, 0);
        std::uint32_t tok = 0;
        t.confirm(0x1234, 0x0006, f.data, f.len, kNoAck, &tok);
        t.looking_up(tok, kIeee);
        for (std::uint8_t i = 0; i < kSlots - 1; i++) t.sent(on_cmd(0x2000 + i, i), false, 10);
        t.sent(on_cmd(0x3000, 1), false, 20);   // full: the oldest frame that is not being looked up goes
        Frame back{};
        std::uint64_t ieee = 0;
        expect("a fresh lookup survives a full table", t.take(tok, &back, &ieee) && ieee == kIeee);
    }
    return g_fail;
}
