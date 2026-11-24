// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: PSRAM is the heap.
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void* heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void* heap_caps_realloc(void* p, size_t n, unsigned) { return std::realloc(p, n); }
inline void heap_caps_free(void* p) { std::free(p); }
