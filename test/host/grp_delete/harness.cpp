// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A group delete cut by a power loss must not leak its slot (review PB-08): the real groups_store.cpp
// over the fake NVS in ../nvs_fake, with a cut after each possible number of writes.
#include "groups_store.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static GrpRecord named(const char* name) {
    GrpRecord r{};
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    return r;
}

int main() {
    grp_store_init();
    for (int cut = 0; cut <= 2; cut++) {
        fake_nvs_clear();
        GrpRecord a = named("a"), b = named("b");
        expect("two groups are created as 1 and 2", grp_create(a) && a.id == 1 && grp_create(b) && b.id == 2);

        fake_nvs_cut(cut);              // the power goes after `cut` writes of the delete
        grp_delete(1);
        fake_nvs_cut(-1);               // the next boot

        GrpRecord found{};
        const bool listed = grp_find(1, found);
        GrpRecord again = named("c");
        const bool made = grp_create(again);
        char what[96];
        std::snprintf(what, sizeof(what), "cut after %d write(s): group 1 is either whole or its slot is free", cut);
        expect(what, listed || (made && again.id == 1));
        GrpRecord all[GRP_MAX_GROUPS];
        const uint16_t n = grp_load_all(all, GRP_MAX_GROUPS);
        std::snprintf(what, sizeof(what), "cut after %d write(s): group 2 is untouched", cut);
        expect(what, grp_find(2, found) && std::strcmp(found.name, "b") == 0 && n >= 2);
    }

    // A record the bitmap does not mark (a save or delete cut between its two writes) is not a group.
    fake_nvs_clear();
    GrpRecord a = named("a");
    fake_nvs_cut(1);
    expect("a create cut after its record write fails", !grp_create(a) && a.id == 1);
    fake_nvs_cut(-1);
    GrpRecord found{};
    expect("its record, which the bitmap does not mark, is not found", !grp_find(1, found));

    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
