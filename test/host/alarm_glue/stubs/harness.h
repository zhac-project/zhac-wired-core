// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the stubs and harness.cpp share: the fake clock, the task's sleep budget, the mutex depth, the trace.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
extern int      g_lock_depth;    // the alarm mutex: >0 while held
extern int      g_notified;      // xTaskNotifyGive pending
extern int      g_sleeps_left;   // sleeps the task may still take before it is stopped
extern uint64_t g_now_us;
void host_yield();   // the task has nothing to do: back to the test until it runs the task again
extern std::vector<std::string> g_trace;   // every side effect, in order
#define OUTSIDE_LOCK(what) do { if (g_lock_depth) { std::fprintf(stderr, "FAIL: %s called under the alarm mutex\n", what); std::abort(); } } while (0)
