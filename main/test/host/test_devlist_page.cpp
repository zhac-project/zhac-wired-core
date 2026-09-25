// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test for the cloud device.list paging order (devlist_page.h).
//
// The bug: device.list wrote every device into one 8 KB buffer and stopped at
// ~25-35 devices, so the cloud never saw the rest. The fix pages the cloud
// reply by IEEE cursor; these tests walk the pages the way the cloud's
// reconcile does and check that every live device arrives exactly once, even
// when devices join or leave between two page requests.
#include "../../devlist_page.h"

#include <algorithm>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) std::printf("ok   %s\n", what);                      \
        else { g_fail++; std::printf("FAIL %s (line %d)\n", what, __LINE__); } \
    } while (0)

struct Slot { uint64_t ieee; bool removed; };

static int next(const std::vector<Slot>& pool, uint64_t after) {
    return devlist_next(pool.size(), after,
        [&](size_t k) { return pool[k].ieee; },
        [&](size_t k) { return !pool[k].removed; });
}

// One page of up to `rows` devices after `cursor`. Returns the page; sets
// `cursor` to the next_cursor, or to 0 with `done` when this was the last page
// (the handler leaves next_cursor out exactly then).
static std::vector<uint64_t> page(const std::vector<Slot>& pool, uint64_t& cursor,
                                  size_t rows, bool& done) {
    std::vector<uint64_t> out;
    uint64_t last = cursor;
    for (;;) {
        int i = next(pool, last);
        if (i < 0) { done = true; cursor = 0; return out; }
        if (out.size() == rows) { done = false; cursor = last; return out; }
        out.push_back(pool[i].ieee);
        last = pool[i].ieee;
    }
}

static std::vector<Slot> make_pool(size_t live, size_t tombstones) {
    std::vector<Slot> pool;
    uint64_t x = 0x00158D0000000000ull;
    for (size_t i = 0; i < live + tombstones; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;   // distinct, unordered
        pool.push_back({x | 1, i >= live});
    }
    std::rotate(pool.begin(), pool.begin() + 17, pool.end());      // tombstones mid-pool
    return pool;
}

static std::vector<uint64_t> live_sorted(const std::vector<Slot>& pool) {
    std::vector<uint64_t> v;
    for (const Slot& s : pool) if (!s.removed) v.push_back(s.ieee);
    std::sort(v.begin(), v.end());
    return v;
}

static void test_walk_covers_all() {
    auto pool = make_pool(60, 5);
    std::vector<uint64_t> got;
    uint64_t cursor = 0;
    bool done = false;
    int pages = 0;
    while (!done) {
        auto p = page(pool, cursor, 7, done);
        got.insert(got.end(), p.begin(), p.end());
        if (++pages > 100) break;
    }
    CHECK(got == live_sorted(pool), "60 devices, 7 per page: every live device once, IEEE order, no tombstones");
    CHECK(pages == 9, "60 devices, 7 per page: 9 pages, the last one without next_cursor");
}

static void test_removal_between_pages() {
    auto pool = make_pool(60, 0);
    uint64_t cursor = 0;
    bool done = false;
    std::vector<uint64_t> got = page(pool, cursor, 10, done);
    // One device already sent and one not yet sent leave the network.
    auto order = live_sorted(pool);
    const uint64_t sent = order[3], unsent = order[40];
    for (Slot& s : pool) if (s.ieee == sent || s.ieee == unsent) s.removed = true;
    while (!done) { auto p = page(pool, cursor, 10, done); got.insert(got.end(), p.begin(), p.end()); }
    std::vector<uint64_t> want = order;
    want.erase(std::remove(want.begin(), want.end(), unsent), want.end());
    CHECK(got == want, "removal between pages skips no live device (a slot-index cursor would)");
}

static void test_join_between_pages() {
    auto pool = make_pool(30, 0);
    uint64_t cursor = 0;
    bool done = false;
    std::vector<uint64_t> got = page(pool, cursor, 10, done);
    const uint64_t after_cursor = cursor + 1, before_cursor = got[0] - 1;
    pool.push_back({after_cursor, false});
    pool.push_back({before_cursor, false});
    while (!done) { auto p = page(pool, cursor, 10, done); got.insert(got.end(), p.begin(), p.end()); }
    CHECK(std::count(got.begin(), got.end(), after_cursor) == 1,
          "device joining above the cursor is listed in the same walk");
    CHECK(std::count(got.begin(), got.end(), before_cursor) == 0,
          "device joining below the cursor waits for its device.added event");
    CHECK(std::is_sorted(got.begin(), got.end()), "pages stay in IEEE order");
}

static void test_edges() {
    std::vector<Slot> empty;
    CHECK(next(empty, 0) == -1, "empty pool: no page rows");
    std::vector<Slot> one{{0x10, false}};
    CHECK(next(one, 0) == 0 && next(one, 0x10) == -1, "cursor equal to the last IEEE ends the walk");
    std::vector<Slot> gone{{0x10, true}};
    CHECK(next(gone, 0) == -1, "tombstoned slot is never listed");
}

int main() {
    test_walk_covers_all();
    test_removal_between_pages();
    test_join_between_pages();
    test_edges();
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
