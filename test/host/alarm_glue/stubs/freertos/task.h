// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: the one task runs in turns with the test (harness.cpp, run_task).
#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
inline BaseType_t xTaskCreate(TaskFunction_t, const char*, unsigned, void*, unsigned, TaskHandle_t* h) { *h = reinterpret_cast<void*>(1); return pdPASS; }
inline void xTaskNotifyGive(TaskHandle_t) { g_notified++; }
// The task waits: woken at once when notified; else the whole wait passes while the test lets it sleep;
// else the test gets control back until it runs the task again.
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t ms) {
    OUTSIDE_LOCK("ulTaskNotifyTake");
    for (;;) {
        if (g_notified) { g_notified = 0; return 1; }
        if (g_sleeps_left > 0) { g_sleeps_left--; g_now_us += static_cast<uint64_t>(ms) * 1000; return 0; }
        host_yield();
    }
}
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4000; }
