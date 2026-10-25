// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The cross-site filter's rules (req_filter.h, review 2026-10-24 HS-01).
#include "req_filter.h"

#include <cstdio>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; std::printf("FAIL: %s\n", m); } } while (0)

int main() {
    // The hub's own page, by name, by address, through a proxy on another port.
    CHECK(req_origin_ok("http://zhac.local", "zhac.local"), "same origin by name");
    CHECK(req_origin_ok("http://192.168.1.127", "192.168.1.127:80"), "same origin by address");
    CHECK(req_origin_ok("https://zhac.home", "zhac.home"), "behind a TLS proxy");
    CHECK(req_origin_ok("http://ZHAC.local", "zhac.local"), "host names are case-insensitive");
    CHECK(req_origin_ok("http://[fe80::1]:80", "[fe80::1]"), "IPv6 literal");
    CHECK(req_origin_ok(nullptr, "zhac.local") && req_origin_ok("", "zhac.local"), "no Origin: a script");
    CHECK(req_origin_ok("http://localhost:5173", "192.168.1.127"), "a loopback dev server");

    // Any other page.
    CHECK(!req_origin_ok("https://example.com", "zhac.local"), "a foreign page");
    CHECK(!req_origin_ok("http://zhac.local.example.com", "zhac.local"), "a look-alike name");
    CHECK(!req_origin_ok("null", "zhac.local"), "an opaque origin");
    CHECK(!req_origin_ok("http://192.168.1.50", "192.168.1.127"), "another LAN host");
    CHECK(!req_origin_ok("http://zhac.local", nullptr), "Origin without Host");

    // Host while sign-in is off (DNS rebinding arrives with its own domain).
    CHECK(req_host_ok("zhac.local", "zhac") && req_host_ok("zhac", "zhac"), "the hub's names");
    CHECK(req_host_ok("zhac.local.", "zhac"), "a fully qualified .local name");
    CHECK(req_host_ok("192.168.1.127:80", "zhac") && req_host_ok("[fe80::1]", "zhac"), "IP literals");
    CHECK(req_host_ok(nullptr, "zhac"), "no Host: HTTP/1.0 tooling");
    CHECK(!req_host_ok("rebind.example.com", "zhac"), "a rebinding domain");
    CHECK(!req_host_ok("zhac.example.com", "zhac"), "the hub's name under a foreign domain");
    CHECK(!req_host_ok("zhacx.local", "zhac"), "a longer name");
    CHECK(!req_host_ok("192.168.1", "zhac") && !req_host_ok("1.2.3.4.5", "zhac"), "not a v4 address");
    CHECK(!req_host_ok("zhac.local", ""), "no hostname known yet: names refused");

    char out[8];
    CHECK(!req_host_part("averyveryverylonghost", out, sizeof(out)), "a host too long for the buffer");
    CHECK(!req_host_part("[fe80::1", out, sizeof(out)), "an unclosed IPv6 bracket");

    std::printf("%s (%d failure(s))\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
