# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mono override of zhac-main-core/components/lua_cjson. The sibling
# gates its real registration behind `IDF_TARGET == esp32p4`; mono
# runs Lua on S3 so we re-register unconditionally, reusing the same
# vendored upstream sources via relative paths.

set(LUA_CJSON_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../zhac-main-core/components/lua_cjson")

idf_component_register(
    SRCS
        "${LUA_CJSON_SRC_DIR}/src/lua_cjson.c"
        "${LUA_CJSON_SRC_DIR}/src/strbuf.c"
        "${LUA_CJSON_SRC_DIR}/src/fpconv.c"
    INCLUDE_DIRS "${LUA_CJSON_SRC_DIR}/src"
    PRIV_REQUIRES lua
)
