// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL lua_engine_check_syntax, cut out of zhac-main-core/components/lua_engine/src/lua_engine.c by run.sh
// (check_fn.inc), on the REAL Lua 5.5 the firmware links (georgik__lua, compiled for the host).
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// The engine's allocator (lua_alloc.c: heap caps and a budget) as the plain C heap.
static void* lua_engine_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) { free(ptr); return NULL; }
    return realloc(ptr, nsize);
}

#include "check_fn.inc"
