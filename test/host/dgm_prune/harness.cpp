// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Group-mirror rows of devices that are no longer paired go at boot (review PB-08), and the native
// group a collection matches exactly (research 2026-11-18 B5): the real main/dgm_store.cpp over the
// fake NVS in ../nvs_fake.
#include "dgm_store.h"
#include "nvs.h"
#include <cstdio>
#include <initializer_list>
#include <utility>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static constexpr uint64_t A = 0xA1, B = 0xB2, GONE = 0xDEAD;
static bool paired(uint64_t ieee) { return ieee == A || ieee == B; }

int main() {
    // The pure operation: the gone device's rows (on two endpoints, one of them first in the table)
    // go, the paired devices' rows stay whole.
    DgmTable t{};
    dgm_tbl_add(t, GONE, 1, 10);
    dgm_tbl_add(t, A, 1, 10);
    dgm_tbl_add(t, GONE, 2, 11);
    dgm_tbl_add(t, B, 1, 12);
    dgm_tbl_add(t, A, 2, 13);
    expect("two rows of the gone device are dropped", dgm_tbl_prune(t, paired) == 2);
    uint16_t g[8];
    expect("the table keeps the three rows of paired devices", t.n == 3);
    expect("device A keeps both endpoints' groups", dgm_tbl_list(t, A, g, 8) == 2);
    expect("device B keeps its group", dgm_tbl_list(t, B, g, 8) == 1 && g[0] == 12);
    expect("the gone device has none", dgm_tbl_list(t, GONE, g, 8) == 0);
    expect("a second prune drops nothing", dgm_tbl_prune(t, paired) == 0);

    // The stored mirror: pruned and saved, so the next load no longer has the rows.
    fake_nvs_clear();
    dgm_store_init();
    dgm_add(GONE, 1, 20);
    dgm_add(A, 1, 21);
    expect("the stored mirror loses the gone device's row", dgm_prune(paired) == 1);
    expect("and keeps the paired one", dgm_list(GONE, g, 8) == 0 && dgm_list(A, g, 8) == 1 && g[0] == 21);

    // The group a collection matches exactly: one groupcast switches it (B5).
    DgmTable m{};
    constexpr uint64_t C = 0xC3, D = 0xD4, E = 0xE5, F = 0xF6, X = 0x99;
    dgm_tbl_add(m, A, 1, 5);
    dgm_tbl_add(m, A, 1, 101);
    dgm_tbl_add(m, B, 1, 101);
    dgm_tbl_add(m, A, 2, 102);
    dgm_tbl_add(m, C, 1, 102);
    for (uint64_t d : {D, E, F}) dgm_tbl_add(m, d, 1, 103);
    const auto of = [&](std::initializer_list<std::pair<uint64_t, uint8_t>> mem) {
        uint64_t ie[8];
        uint8_t  ep[8];
        uint8_t  n = 0;
        for (auto [i, e] : mem) { ie[n] = i; ep[n++] = e; }
        return dgm_tbl_group_of(m, ie, ep, n);
    };
    expect("a group holding exactly the collection's members is found", of({{A, 1}, {B, 1}}) == 101);
    expect("  in any member order", of({{B, 1}, {A, 1}}) == 101);
    expect("a member outside the group: none", of({{A, 1}, {B, 1}, {C, 1}}) == 0);
    expect("the group holds a device outside the collection: none (it would switch too)", of({{D, 1}, {E, 1}}) == 0);
    expect("endpoints count: A's second endpoint and C are group 102", of({{A, 2}, {C, 1}}) == 102);
    expect("  A's first endpoint is not in 102: none", of({{A, 1}, {C, 1}}) == 0);
    expect("a member listed twice: none", of({{A, 1}, {A, 1}, {B, 1}}) == 0);
    expect("a device the mirror does not know: none", of({{X, 1}, {B, 1}}) == 0);
    expect("no members: none", of({}) == 0);
    fake_nvs_clear();
    dgm_add(A, 1, 7);
    dgm_add(B, 1, 7);
    const uint64_t ab[2] = {A, B};
    const uint8_t  ep1[2] = {1, 1};
    expect("the stored mirror answers the same", dgm_group_of(ab, ep1, 2) == 7);

    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
