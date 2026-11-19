// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Raw APS sends waiting for their confirm (research 2026-11-18 B1): a frame
// the stack could not deliver is handed back once, for an address lookup and
// one resend; the resend failing is the end of it.
#include "../../esp_zb_tx_track.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

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

// A frame to endpoint `ep` of 0x1234, byte for byte.
static Frame raw(std::uint16_t cluster, std::initializer_list<std::uint8_t> bytes, std::uint8_t ep = 1) {
    Frame f{};
    f.nwk = 0x1234; f.ep = ep; f.cluster = cluster;
    for (std::uint8_t b : bytes) f.data[f.len++] = b;
    return f;
}
// `f` failed once and its address lookup ended: is it sent again?
static bool sent_again(Table& t, const Frame& f) {
    std::uint32_t tok = 0;
    if (t.confirm(f.nwk, f.cluster, f.data, f.len, kNoAck, &tok) != Verdict::kRetry) return false;
    t.looking_up(tok, kIeee);
    Frame back{};
    std::uint64_t ieee = 0;
    bool again = false;
    return t.take(tok, &back, &ieee, &again) && again;
}

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
        Table t;   // two reads: neither makes the other out of date (an On would, review P4 #3)
        const Frame a = raw(0x0006, {0x00, 7, 0x00, 0x00, 0x00}), b = raw(0x0006, {0x00, 8, 0x00, 0x00, 0x00});
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
        // The stack copies the sent bytes into every confirm; a frame the hub
        // did not track (an earlier one, cancelled, or forgotten) is not
        // another frame's failure.
        Table t;
        const Frame f = on_cmd(0x1234, 7), other = on_cmd(0x1234, 99);
        t.sent(f, false, 0);
        std::uint32_t tok = 0;
        expect("a confirm carrying other bytes: not ours",
               t.confirm(0x1234, 0x0006, other.data, other.len, kNoAck, &tok) == Verdict::kNone);
        expect("  ours still waits for its own", t.confirm(0x1234, 0x0006, f.data, f.len, kNoAck, &tok) == Verdict::kRetry);
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

    // Sent again only when that changes nothing (review 2026-11-19 P4 #3): a NO_ACK may mean only the acks were
    // lost, and the resend goes out seconds later.
    {
        Table t;
        const Frame toggle = raw(0x0006, {0x01, 1, 0x02});
        t.sent(toggle, false, 0);
        std::uint32_t tok = 0;
        expect("a Toggle not delivered: looked up all the same (offline, a new address)",
               t.confirm(0x1234, 0x0006, toggle.data, toggle.len, kNoAck, &tok) == Verdict::kRetry);
        t.looking_up(tok, kIeee);
        Frame back{};
        std::uint64_t ieee = 0;
        bool again = true;
        expect("  but not sent again: the device may have toggled already", t.take(tok, &back, &ieee, &again) && !again);
    }
    {
        Table t;
        const Frame on = raw(0x0006, {0x01, 1, 0x01});
        t.sent(on, false, 0);
        expect("an On with nothing newer: sent again", sent_again(t, on));
    }
    {
        struct { std::uint16_t cluster; std::uint8_t cmd; const char* what; } once[] = {
            {0x0008, 0x02, "level Step"}, {0x0008, 0x06, "level Step with On/Off"}, {0x0300, 0x02, "Step Hue"},
            {0x0300, 0x05, "Step Saturation"}, {0x0300, 0x09, "Step Color"}, {0x0300, 0x42, "Enhanced Step Hue"},
            {0x0300, 0x4C, "Step Color Temperature"}, {0x0201, 0x00, "Setpoint Raise/Lower"}};
        bool none = true;
        for (const auto& o : once) {
            Table t;
            const Frame f = raw(o.cluster, {0x11, 1, o.cmd, 0x00, 0x10, 0x00});   // 0x11: no default response
            t.sent(f, false, 0);
            if (sent_again(t, f)) { none = false; std::printf("     sent again: %s\n", o.what); }
        }
        expect("steps and relative changes (level, colour, setpoint): never sent again", none);
        Table t;
        const Frame to_level = raw(0x0008, {0x01, 1, 0x04, 0x80, 0x00, 0x00});
        t.sent(to_level, false, 0);
        expect("Move to Level (to an absolute level): sent again", sent_again(t, to_level));
        const Frame read = raw(0x0006, {0x00, 2, 0x00, 0x00, 0x00});
        t.sent(read, false, 10);
        expect("a read (a global command): sent again", sent_again(t, read));
        const Frame mfr = raw(0x0006, {0x05, 0x34, 0x02, 3, 0x02});   // manufacturer 0x0234, its own command ids
        t.sent(mfr, false, 20);
        expect("a manufacturer-specific command: sent again, as before", sent_again(t, mfr));
    }
    {   // A newer command to the same endpoint and cluster makes an older one out of date.
        Table t;
        const Frame on = raw(0x0006, {0x01, 1, 0x01}), off = raw(0x0006, {0x01, 2, 0x00});
        t.sent(on, false, 0);
        t.sent(off, false, 10);
        std::uint32_t tok = 0;
        expect("an On, then an Off: the On's failure is let go, no lookup (the Off's own confirm decides)",
               t.confirm(0x1234, 0x0006, on.data, on.len, kNoAck, &tok) == Verdict::kNone);
        expect("  the Off failing: sent again", sent_again(t, off));
    }
    {
        Table t;
        const Frame on = raw(0x0006, {0x01, 1, 0x01}), off = raw(0x0006, {0x01, 2, 0x00});
        t.sent(on, false, 0);
        std::uint32_t tok = 0;
        t.confirm(0x1234, 0x0006, on.data, on.len, kNoAck, &tok);
        t.looking_up(tok, kIeee);
        t.sent(off, false, 10);   // the person's Off while the On is looked up
        Frame back{};
        std::uint64_t ieee = 0;
        bool again = true;
        expect("an Off sent while the On is looked up: the On is not sent after it",
               t.take(tok, &back, &ieee, &again) && !again);
    }
    {
        Table t;
        const Frame on1 = raw(0x0006, {0x01, 1, 0x01}, 1), off2 = raw(0x0006, {0x01, 2, 0x00}, 2);
        t.sent(on1, false, 0);
        t.sent(off2, false, 10);
        expect("an Off to another endpoint (a two-gang switch): the On is still sent again", sent_again(t, on1));
        const Frame read = raw(0x0006, {0x00, 3, 0x00, 0x00, 0x00}), on = raw(0x0006, {0x01, 4, 0x01});
        t.sent(read, false, 20);
        t.sent(on, false, 30);
        expect("a read is not made out of date by a newer command", sent_again(t, read));
    }
    {   // Tuya data frames (0xEF00): [fc][tsn][cmd][seq:2][dp][type][len:2][value]; the data point is the target.
        Table t;
        const Frame dp1_on = raw(0xEF00, {0x01, 1, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01});
        const Frame dp2_set = raw(0xEF00, {0x01, 2, 0x00, 0x00, 0x02, 0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0xD2});
        t.sent(dp1_on, false, 0);
        t.sent(dp2_set, false, 10);
        expect("Tuya: a write to another data point (mode, then setpoint): the first is still sent again",
               sent_again(t, dp1_on));
        const Frame dp1_off = raw(0xEF00, {0x01, 3, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x01, 0x00});
        t.sent(dp1_on, false, 20);
        t.sent(dp1_off, false, 30);
        std::uint32_t tok = 0;
        expect("Tuya: the same data point written again: the older write is let go",
               t.confirm(0x1234, 0xEF00, dp1_on.data, dp1_on.len, kNoAck, &tok) == Verdict::kNone);
    }
    return g_fail;
}
