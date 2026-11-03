// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// An unknown device joins the pool only when the trust centre let it in a
// moment ago, or while permit-join is open (plus a grace period), and only so
// many per window (review 2026-10-24 ZR-02).
#include "../../esp_zb_admit.h"

#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static constexpr int64_t S = 1000000LL;   // one second in us
using V = AdmitGate::Verdict;

int main() {
    const uint64_t a = 0xA4C1380000000001ull, b = 0xA4C1380000000002ull;
    {
        AdmitGate g;
        expect("an announce at boot, nothing authorised, network closed: refused",
               g.admit(a, 2 * S) == V::NOT_AUTHORISED);
        g.permit_join(0, 3 * S);
        expect("closing a network that was not open opens nothing", g.admit(a, 4 * S) == V::NOT_AUTHORISED);

        g.authorised(a, 10 * S);
        expect("authorised by the trust centre: admitted", g.admit(a, 11 * S) == V::ADMIT);
        expect("  another address is not", g.admit(b, 11 * S) == V::NOT_AUTHORISED);
        expect("  the authorisation expires",
               g.admit(a, 10 * S + AdmitGate::kAuthTtlUs + S) == V::NOT_AUTHORISED);
    }
    {
        AdmitGate g;
        const int64_t t0 = 100 * S;
        g.permit_join(254, t0);
        expect("permit-join open: an unknown device is admitted", g.admit(a, t0 + 100 * S) == V::ADMIT);
        expect("  and for the grace period after it closes",
               g.admit(b, t0 + 254 * S + AdmitGate::kGraceUs) == V::ADMIT);
        expect("  not after", g.admit(b, t0 + 254 * S + AdmitGate::kGraceUs + S) == V::NOT_AUTHORISED);
    }
    {
        AdmitGate g;
        const int64_t t0 = 100 * S;
        g.permit_join(254, t0);
        g.permit_join(0, t0 + 10 * S);   // the owner closed it early
        expect("closed early: the grace runs from the close",
               g.admit(a, t0 + 10 * S + AdmitGate::kGraceUs) == V::ADMIT &&
               g.admit(b, t0 + 10 * S + AdmitGate::kGraceUs + S) == V::NOT_AUTHORISED);
    }
    {
        AdmitGate g;
        const int64_t t0 = 100 * S;
        g.permit_join(254, t0);
        int admitted = 0;
        for (int i = 0; i < AdmitGate::kCap + 5; i++)
            admitted += g.admit(0x1000 + i, t0 + i * S) == V::ADMIT;
        expect("no more than kCap new devices per window", admitted == AdmitGate::kCap);
        expect("  the next one is capped", g.admit(0x2000, t0 + 50 * S) == V::CAPPED);
        g.authorised(0x2001, t0 + AdmitGate::kWindowUs);
        expect("  a new window admits again", g.admit(0x2001, t0 + AdmitGate::kWindowUs + S) == V::ADMIT);
    }
    {
        AdmitGate g;
        for (int i = 0; i < AdmitGate::kAuthSlots + 1; i++) g.authorised(0x3000 + i, S);
        expect("the newest authorisations are kept", g.admit(0x3000 + AdmitGate::kAuthSlots, 2 * S) == V::ADMIT);
        expect("  the oldest roll off", g.admit(0x3000, 2 * S) == V::NOT_AUTHORISED);
        g.authorised(0x4000, 3 * S);
        g.authorised(0x4000, 4 * S);   // authorised again: one slot, refreshed
        expect("  the same device authorised twice keeps one slot (evicts no other)",
               g.admit(0x3002, 5 * S) == V::ADMIT && g.admit(0x4000, 5 * S) == V::ADMIT);
    }

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nadmit gate OK\n", g_fail);
    return g_fail;
}
