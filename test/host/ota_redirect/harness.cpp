// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Following an update URL's redirects (main/ota_redirect.h, review F09, F33, 2026-10-24 F-04): every hop
// stays on https://, at most kOtaMaxHops of them, a loop ends instead of holding the updater for ever, and a
// Location keeps its query (a signed download URL carries its signature there). The "server" is a table
// of URL -> status / Location. The same checks as zhac-net-core/test/host/test_ota_redirect.cpp.
#include "ota_redirect.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

struct Reply { int status; std::string location; };
static std::map<std::string, Reply> g_web;
static int g_fetches = 0;
static int fetch(const char* url, char* next, size_t cap) {
    g_fetches++;
    auto it = g_web.find(url);
    if (it == g_web.end()) return -1;
    if (it->second.location.size() >= cap) return -1;               // the contract: a Location that does not fit is a failure
    std::snprintf(next, cap, "%s", it->second.location.c_str());   // the Location value as sent
    return it->second.status;
}

static OtaResolve run(const char* url, std::string* out, size_t cap = 1536) {
    char buf[1536], next[1536];
    g_fetches = 0;
    const OtaResolve r = ota_resolve_redirects(url, buf, next, cap, fetch);
    *out = buf;
    return r;
}

int main() {
    std::string out;
    g_web["https://github.com/a.bin"] = {302, "https://objects.example/a.bin?sig=x"};
    g_web["https://objects.example/a.bin?sig=x"] = {200, ""};
    CHECK(run("https://github.com/a.bin", &out) == OtaResolve::OK && out == "https://objects.example/a.bin?sig=x",
          "GitHub-style hop ends at the signed URL");

    g_web["https://evil.example/b.bin"] = {301, "http://plain.example/b.bin"};
    CHECK(run("https://evil.example/b.bin", &out) == OtaResolve::INSECURE, "a redirect to http:// is refused");
    CHECK(g_fetches == 1, "  before anything is fetched from it");
    CHECK(run("http://plain.example/b.bin", &out) == OtaResolve::INSECURE, "an http:// start is refused");
    g_web["https://evil.example/c.bin"] = {302, "ftp://plain.example/c.bin"};
    CHECK(run("https://evil.example/c.bin", &out) == OtaResolve::INSECURE, "a redirect to another scheme is refused");

    for (int i = 0; i < 7; i++)
        g_web["https://chain/" + std::to_string(i)] = {307, "https://chain/" + std::to_string(i + 1)};
    g_web["https://chain/7"] = {200, ""};
    CHECK(run("https://chain/0", &out) == OtaResolve::TOO_MANY, "seven hops are too many");
    g_web["https://chain/2"] = {200, ""};
    CHECK(run("https://chain/0", &out) == OtaResolve::OK && out == "https://chain/2", "two hops are fine");

    g_web["https://loop/a"] = {302, "https://loop/b"};
    g_web["https://loop/b"] = {302, "https://loop/a"};
    CHECK(run("https://loop/a", &out) == OtaResolve::TOO_MANY, "a redirect loop ends");
    CHECK(g_fetches <= kOtaMaxHops + 1, "  after a bounded number of requests");

    CHECK(run("https://nowhere/x", &out) == OtaResolve::FAILED, "an unreachable server is a failure");
    g_web["https://hub/404"] = {404, ""};
    CHECK(run("https://hub/404", &out) == OtaResolve::OK, "a non-redirect status ends the walk");

    // A Location need not be absolute, and its query must survive: it is the signature of a signed URL.
    g_web["https://rel.example:8443/a/b.bin?x=1"] = {302, "/c.bin?sig=abc&e=2"};
    g_web["https://rel.example:8443/c.bin?sig=abc&e=2"] = {200, ""};
    CHECK(run("https://rel.example:8443/a/b.bin?x=1", &out) == OtaResolve::OK &&
          out == "https://rel.example:8443/c.bin?sig=abc&e=2", "an absolute-path Location keeps host, port and query");
    g_web["https://rel.example"] = {302, "/d.bin"};
    g_web["https://rel.example/d.bin"] = {200, ""};
    CHECK(run("https://rel.example", &out) == OtaResolve::OK && out == "https://rel.example/d.bin",
          "  also from a URL with no path");
    g_web["https://rel.example?q=1"] = {302, "/d.bin"};
    CHECK(run("https://rel.example?q=1", &out) == OtaResolve::OK && out == "https://rel.example/d.bin",
          "  or with only a query");
    g_web["https://sr.example/a"] = {302, "//cdn.example/c.bin?s=1"};
    g_web["https://cdn.example/c.bin?s=1"] = {200, ""};
    CHECK(run("https://sr.example/a", &out) == OtaResolve::OK && out == "https://cdn.example/c.bin?s=1",
          "a scheme-relative Location keeps https and the query");
    g_web["https://abs.example/a"] = {302, "HTTPS://Cdn.Example/c.bin"};
    g_web["HTTPS://Cdn.Example/c.bin"] = {200, ""};
    CHECK(run("https://abs.example/a", &out) == OtaResolve::OK && out == "HTTPS://Cdn.Example/c.bin",
          "the scheme is case-insensitive");

    g_web["https://bad.example/a"] = {302, "c.bin"};
    CHECK(run("https://bad.example/a", &out) == OtaResolve::FAILED, "a path-relative Location is refused");
    g_web["https://bad.example/b"] = {302, ""};
    CHECK(run("https://bad.example/b", &out) == OtaResolve::FAILED, "a redirect with no Location is a failure");
    g_web["https://bad.example/c"] = {302, "https://ok.example/a b"};
    g_web["https://ok.example/a b"] = {200, ""};
    CHECK(run("https://bad.example/c", &out) == OtaResolve::FAILED && g_fetches == 1,
          "a Location with a space is refused before it is fetched");
    g_web["https://bad.example/d"] = {302, "https://ok.example/a\x01" "b"};
    g_web["https://ok.example/a\x01" "b"] = {200, ""};
    CHECK(run("https://bad.example/d", &out) == OtaResolve::FAILED && g_fetches == 1,
          "  and so is one with a control character");
    g_web["https://bad.example/e"] = {302, "https://" + std::string(100, 'h') + "/x"};
    CHECK(run("https://bad.example/e", &out, 64) == OtaResolve::FAILED, "a Location longer than the buffer is refused");
    CHECK(run((std::string("https://") + std::string(100, 'h')).c_str(), &out, 64) == OtaResolve::FAILED,
          "so is a start URL that does not fit");
    g_web["https://bad.example/f"] = {302, "/" + std::string(60, 'p')};
    CHECK(run("https://bad.example/f", &out, 64) == OtaResolve::FAILED,
          "an absolute-path Location that overflows once the host is added is refused");

    printf(s_fail ? "\n%d FAILURE(S)\n" : "\nota redirects OK\n", s_fail);
    return s_fail ? 1 : 0;
}
