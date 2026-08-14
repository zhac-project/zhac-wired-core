# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mono override of zhac-main-core/components/lua_engine.
#
# The sibling component gates its real registration behind
# `if(IDF_TARGET STREQUAL "esp32p4")` and registers an EMPTY component
# for every other target — including S3. That gate dates from the
# dual-chip era where Lua only ever ran on P4. Mono runs Lua on S3,
# so we re-register with the same SRCS/include layout but unconditional,
# and reference the sibling source files via relative paths so no
# code is duplicated.
#
# The PRIV_REQUIRES list mirrors the sibling's, minus dual-chip-only
# deps that aren't reachable in mono.

set(LUA_ENGINE_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../zhac-main-core/components/lua_engine")

idf_component_register(
    SRCS
        "${LUA_ENGINE_SRC_DIR}/src/lua_alloc.c"
        "${LUA_ENGINE_SRC_DIR}/src/lua_engine.c"
        "${LUA_ENGINE_SRC_DIR}/src/lua_scheduler.cpp"
        "${LUA_ENGINE_SRC_DIR}/src/lua_sandbox.c"
        "${LUA_ENGINE_SRC_DIR}/src/lua_require.c"
        "${LUA_ENGINE_SRC_DIR}/src/zhac_lua_module.cpp"
        "${LUA_ENGINE_SRC_DIR}/src/lua_engine_event_bridge.cpp"
        "${LUA_ENGINE_SRC_DIR}/src/lua_engine_rules_hook.cpp"
        "${LUA_ENGINE_SRC_DIR}/src/lua_script_cache.cpp"
    INCLUDE_DIRS "${LUA_ENGINE_SRC_DIR}/include"
    PRIV_INCLUDE_DIRS "${LUA_ENGINE_SRC_DIR}/src"
    REQUIRES freertos esp_timer lua
    PRIV_REQUIRES spiffs event_bus metrics
                  zap_common zap_store zhc_adapter
                  device_shadow mqtt_gw tg_gw zigbee_mgr
                  simple_rules lua_cjson
)
