// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: one mutex, the alarm panel's. Taking it twice is a deadlock on the hub and aborts here.
#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new int(0); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
    if (g_lock_depth++) { std::fprintf(stderr, "FAIL: alarm mutex taken twice (deadlock on the hub)\n"); std::abort(); }
    return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { g_lock_depth--; return pdTRUE; }
