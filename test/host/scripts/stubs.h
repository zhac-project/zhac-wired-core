// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-ins for what the script ops in main/ws_bridge.cpp touch outside themselves:
// the heap caps allocator and the clock, the WebSocket reply sink and push, the script store (SPIFFS), the Lua
// engine's reload / run, and PSA's one-shot hash. Everything else is the real code: JsonWriter, utf8_seq,
// remote_xfer (the staging slot and the page cutter), remote_allow (remote_script_name_ok), lua_engine_err.h,
// ArduinoJson, and the real lua_engine_check_syntax on real Lua 5.5 (lua_check.c).
#pragma once
#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <ArduinoJson.h>

#include "lua_engine.h"           // real: LuaReloadResult, lua_engine_check_syntax, lua_engine_reload
#include "lua_engine_scripts.h"   // real: LuaScriptEntry, LUA_SCRIPT_MAX / _NAME_MAX / _SRC_MAX, lua_script_cache_*
#include "lua_engine_err.h"       // real: lua_err_parse
#include "remote_allow.h"         // real: remote_script_name_ok
#include "remote_xfer.h"          // real: remote_xfer_part / use / page, kRemotePageBudget, kRemoteXferMax

// heap_caps: malloc/free, with the Nth call (1-based, counted from g_alloc_calls = 0) made to fail when
// g_alloc_fail_at > 0, or every call from the Nth on when g_alloc_fail_from > 0
#define MALLOC_CAP_SPIRAM 1
void* heap_caps_malloc(size_t size, uint32_t caps);
void  heap_caps_free(void* p);

// the monotonic clock behind now_ms(): the harness moves it
int64_t esp_timer_get_time();

// the reply sink: every frame the code under test sends
void ws_server_reply(int fd, const char* buf, size_t len);

// pushes: ws_push (script.added / updated / deleted) and push_event (script.error), captured
void ws_push(const char* event, JsonDocument& data);

// PSA one-shot SHA-256 (OpenSSL underneath)
typedef int32_t psa_status_t;
constexpr psa_status_t PSA_SUCCESS = 0;
#define PSA_ALG_SHA_256 ((uint32_t)0x02000009)
psa_status_t psa_hash_compute(uint32_t alg, const uint8_t* input, size_t input_length, uint8_t* hash,
                              size_t hash_size, size_t* hash_length);
