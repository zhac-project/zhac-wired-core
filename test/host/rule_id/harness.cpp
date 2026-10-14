// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Rule ids in REST paths (review F34): exactly a decimal 1-65535, then the end
// of the path, "/enable" or a query. Anything else is 0 = not found, so a
// malformed path can never address another rule (65537 used to mean rule 1).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "slice.inc"   // parse_id_from_uri, cut from main/api_rules.cpp

static int g_fail = 0;
static void expect(const char* uri, unsigned want) {
    const unsigned got = parse_id_from_uri(uri, "/api/rules/");
    std::printf("%s %-28s -> %u\n", got == want ? "ok  " : "FAIL", uri, got);
    if (got != want) g_fail++;
}

int main() {
    expect("/api/rules/1", 1);
    expect("/api/rules/42/enable", 42);
    expect("/api/rules/7?x=1", 7);
    expect("/api/rules/65535", 65535);
    expect("/api/rules/65536", 0);
    expect("/api/rules/65537", 0);
    expect("/api/rules/0", 0);
    expect("/api/rules/-1", 0);
    expect("/api/rules/+3", 0);
    expect("/api/rules/ 3", 0);
    expect("/api/rules/3abc", 0);
    expect("/api/rules/", 0);
    expect("/api/rules/99999999999999999999", 0);
    expect("/api/other/3", 0);
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nrule ids OK\n", g_fail);
    return g_fail;
}
