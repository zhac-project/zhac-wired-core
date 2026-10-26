// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Group-mirror rows of devices that are no longer paired go at boot (review PB-08): the real
// main/dgm_store.cpp over the fake NVS in ../nvs_fake.
#include "dgm_store.h"
#include "nvs.h"
#include <cstdio>

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

    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
