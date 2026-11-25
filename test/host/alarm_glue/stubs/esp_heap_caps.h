// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: PSRAM and internal RAM are the heap; heap_asked() lists what each allocation asked for, as (size, caps).
#pragma once
#include <cstdlib>
#include <utility>
#include <vector>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
inline std::vector<std::pair<size_t, unsigned>>& heap_asked() {
    static std::vector<std::pair<size_t, unsigned>> v;
    return v;
}
inline void* heap_caps_malloc(size_t n, unsigned caps) {
    heap_asked().emplace_back(n, caps);
    return std::malloc(n);
}
inline void* heap_caps_realloc(void* p, size_t n, unsigned) { return std::realloc(p, n); }
inline void heap_caps_free(void* p) { std::free(p); }
