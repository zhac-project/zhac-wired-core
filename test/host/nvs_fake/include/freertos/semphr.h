// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// One task: the store's lock is a no-op.
#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { static int m; return &m; }
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdTRUE; }
