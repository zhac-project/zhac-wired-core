// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub.
#pragma once
#include "harness.h"
typedef int BaseType_t; typedef unsigned UBaseType_t; typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
#define configASSERT(x) do { if (!(x)) std::abort(); } while (0)
