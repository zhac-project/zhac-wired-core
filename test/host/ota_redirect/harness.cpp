// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Following an update URL's redirects (review F09, F33): every hop stays on
// https://, at most kMaxHops of them, and a loop ends instead of holding the
// updater forever. The "server" is a table of URL -> status / Location.
#include "ota_redirect.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

struct Reply { int status; std::string location; };
static std::map<std::string, Reply> g_web;
static int g_fetches = 0;
static int fetch(const char* url, char* next, size_t cap) {
    g_fetches++;
    auto it = g_web.find(url);
    if (it == g_web.end()) return -1;
    std::snprintf(next, cap, "%s", it->second.location.c_str());
    return it->second.status;
}

static OtaResolve run(const char* url, std::string* out) {
    char buf[1536], next[1536];
    g_fetches = 0;
    const OtaResolve r = ota_resolve_redirects(url, buf, next, sizeof(buf), fetch);
    *out = buf;
    return r;
}

int main() {
    std::string out;
    g_web["https://github.com/a.bin"] = {302, "https://objects.example/a.bin?sig=x"};
    g_web["https://objects.example/a.bin?sig=x"] = {200, ""};
    expect("GitHub-style hop ends at the signed URL",
           run("https://github.com/a.bin", &out) == OtaResolve::OK && out == "https://objects.example/a.bin?sig=x");

    g_web["https://evil.example/b.bin"] = {301, "http://plain.example/b.bin"};
    expect("a redirect to http:// is refused", run("https://evil.example/b.bin", &out) == OtaResolve::INSECURE);
    expect("  before anything is fetched from it", g_fetches == 1);
    expect("an http:// start is refused", run("http://plain.example/b.bin", &out) == OtaResolve::INSECURE);

    for (int i = 0; i < 7; i++)
        g_web["https://chain/" + std::to_string(i)] = {307, "https://chain/" + std::to_string(i + 1)};
    g_web["https://chain/7"] = {200, ""};
    expect("seven hops are too many", run("https://chain/0", &out) == OtaResolve::TOO_MANY);
    g_web["https://chain/2"] = {200, ""};
    expect("two hops are fine", run("https://chain/0", &out) == OtaResolve::OK && out == "https://chain/2");

    g_web["https://loop/a"] = {302, "https://loop/b"};
    g_web["https://loop/b"] = {302, "https://loop/a"};
    expect("a redirect loop ends", run("https://loop/a", &out) == OtaResolve::TOO_MANY);
    expect("  after a bounded number of requests", g_fetches <= kOtaMaxHops + 1);

    expect("an unreachable server is a failure", run("https://nowhere/x", &out) == OtaResolve::FAILED);
    g_web["https://hub/404"] = {404, ""};
    expect("a non-redirect status ends the walk", run("https://hub/404", &out) == OtaResolve::OK);

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nota redirects OK\n", g_fail);
    return g_fail;
}
