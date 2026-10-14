// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A reply belongs to the request still waiting for it (review F05, F06): the
// interview and bind waits share one slot each, and the SDK can deliver a
// reply after its waiter gave up. Replayed here in the order that broke them.
#include "../../esp_zb_req_gen.h"

#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    ReqGen g;
    expect("nothing is current before a request", !g.current(nullptr));

    void* node_desc = g.begin();
    expect("the reply to the request in flight is current", g.current(node_desc));

    g.expire();                                    // node descriptor timed out
    void* active_ep = g.begin();                   // next step goes out
    expect("a late node-descriptor reply is not current", !g.current(node_desc));
    expect("the active-endpoint reply is", g.current(active_ep));

    g.expire();
    expect("after a timeout nothing is current", !g.current(active_ep));

    void* a = g.begin();
    void* b = g.begin();                           // a second request replaces the first
    expect("only the newest request is current", !g.current(a) && g.current(b));

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nrequest matching OK\n", g_fail);
    return g_fail;
}
