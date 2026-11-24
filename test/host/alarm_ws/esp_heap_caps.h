// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-in for the heap caps allocator: psram_json.h's allocator and the code under test take their memory
// here. harness.cpp counts every call and can make one of them fail.
#pragma once
#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_SPIRAM 1
void* heap_caps_malloc(size_t size, uint32_t caps);
void* heap_caps_realloc(void* p, size_t size, uint32_t caps);
void  heap_caps_free(void* p);
