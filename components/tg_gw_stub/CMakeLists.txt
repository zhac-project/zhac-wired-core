# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# tg_gw_stub -- fills a target gap in the sibling tg_gw component.
#
# tg_gw/CMakeLists.txt selects its source by target:
#
#     if(IDF_TARGET STREQUAL "esp32p4")      -> tg_gw_p4.cpp
#     elseif(IDF_TARGET STREQUAL "esp32s3")  -> tg_gw_s3.cpp
#     endif()                                 <-- no else
#
# On esp32s31 that registers the component with ZERO sources, so tg_gw_send /
# _setchat / _settoken vanish and lua_engine's zhac_lua_module.cpp fails to
# link. (mqtt_gw has the same shape but does have an else, so it links -- with
# the dual-chip P4 variant, which is its own wrongness for a single-chip SKU.)
#
# Fixing tg_gw properly is a change to zhac-components, which this repo treats
# as a read-only input -- so it is RAISED, not made here. See
# ../extra/docs/WIRED_CORE_MULTI_TARGET_DESIGN.md.
#
# Deliberately built ONLY where the sibling supplies nothing. On esp32p4 the
# sibling's real tg_gw_p4.cpp is present and adding these would be duplicate
# symbols at link time.
if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s3")
    idf_component_register()
else()
    idf_component_register(SRCS "tg_gw_stub.cpp" REQUIRES tg_gw)
endif()
