// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Paging order for the cloud's device.list.
//
// Pages walk the pool in IEEE order and the cursor is the last IEEE sent, so a
// cursor stays valid when devices join or leave between two page requests. A
// pool-index cursor would skip a device whenever an earlier slot is freed, and
// the cloud's reconcile then deletes the skipped device from its cache as
// absent. Header-only and free of ESP-IDF so it runs in a host test.
#pragma once

#include <cstddef>
#include <cstdint>

// Slot of the live device with the smallest IEEE strictly greater than
// `after`, or -1 when none is left. `after` = 0 starts from the first device.
// ponytail: O(pool) per row, O(pool²) per full walk (200 slots ≈ 40k compares
// under the pool lock, microseconds); sort a slot index if the pool grows a lot.
template <typename IeeeOf, typename Live>
int devlist_next(size_t n, uint64_t after, IeeeOf ieee_of, Live live) {
    int best = -1;
    uint64_t best_ieee = 0;
    for (size_t i = 0; i < n; i++) {
        if (!live(i)) continue;
        const uint64_t v = ieee_of(i);
        if (v > after && (best < 0 || v < best_ieee)) {
            best = static_cast<int>(i);
            best_ieee = v;
        }
    }
    return best;
}
