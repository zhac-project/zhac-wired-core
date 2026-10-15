// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// One lock for every rule and script change on this hub. The hub page, REST and the cloud relay run on
// different tasks; a save with a `base` fingerprint compares and writes under this lock, so no other save
// lands between the two (review F22) -- and every other save takes it too, or one still could.
// Recursive: a handler may call another that takes it.
//   EditLock lock(g_edit_mutex);
#pragma once

#include <mutex>

inline std::recursive_mutex g_edit_mutex;
using EditLock = std::lock_guard<std::recursive_mutex>;
