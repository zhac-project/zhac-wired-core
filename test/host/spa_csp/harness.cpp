// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The web UI's Content-Security-Policy (review HS-14): the WebSocket may go to the hub's own host only,
// and a Host header that is not a plain name or address never reaches the policy.
#include "spa_csp.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static std::string csp(const char* host) {
    char out[kSpaCspMax];
    spa_csp(host, out, sizeof(out));
    return out;
}
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static bool any_host_ws(const std::string& s) { return has(s, " ws: ") || has(s, " wss: ") || has(s, " ws:;") || has(s, " wss:;"); }

int main() {
    const std::string ip = csp("192.168.1.5");
    expect("an IPv4 Host: connect-src names it for ws and wss",
           has(ip, "connect-src 'self' ws://192.168.1.5 wss://192.168.1.5 https://api.github.com;"));
    expect("  and no WebSocket to any host", !any_host_ws(ip));
    expect("  the rest of the policy stays", has(ip, "default-src 'self';") && has(ip, "frame-ancestors 'none'") &&
                                             has(ip, "script-src 'self';"));
    expect("a name with a port", has(csp("zhac.local:8080"), "ws://zhac.local:8080 wss://zhac.local:8080 "));
    const char* self_only = "connect-src 'self' https://api.github.com;";
    expect("an IPv6 literal: 'self' only", has(csp("[fe80::1]"), self_only));
    expect("a Host with policy text in it: 'self' only", has(csp("x; script-src *"), self_only) &&
                                                         !has(csp("x; script-src *"), "script-src *"));
    expect("no Host: 'self' only", has(csp(""), self_only) && has(csp(nullptr), self_only));
    const std::string longest(63, 'a');
    const std::string l = csp(longest.c_str());
    expect("the longest Host fits whole", has(l, ("wss://" + longest + " https://api.github.com;").c_str()) &&
                                          has(l, "frame-ancestors 'none'"));
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
