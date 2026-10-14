// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// An ArduinoJson allocator in PSRAM, for documents that grow with the number
// of devices: internal RAM is the scarce kind on these boards.
#pragma once

#include "ArduinoJson.h"
#include "esp_heap_caps.h"

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};

inline ArduinoJson::Allocator* psram_json() {
    static PsramAllocator a;
    return &a;
}
