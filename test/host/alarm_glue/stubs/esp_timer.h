// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: the fake clock.
#pragma once
#include "harness.h"
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(g_now_us); }
