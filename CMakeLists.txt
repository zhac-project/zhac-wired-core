# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# zhac-wired-core — single-chip ZHAC firmware (ESP32-P4 + wired Ethernet).
# Sibling repos are read-only inputs; the bridge shim under components/
# overrides hap_master and hap_slave so the SPI-bridge symbols become
# local function calls. Local components/ wins by ESP-IDF's "first match
# by name" rule.
cmake_minimum_required(VERSION 3.22)

# NOTE (Tasks 1-3): the sibling component dirs are deliberately NOT on the
# search path yet.
#
# ESP-IDF only *builds* components reachable from main's REQUIRES, but it
# *resolves requirements* for every component it discovers. Adding these dirs
# before we need them therefore drags the whole ZHAC dependency graph into
# configure -- and it fails, because lua_cjson requires the registry component
# `lua`, which only arrives with main/idf_component.yml in Task 4:
#
#   Failed to resolve component 'lua' required by component 'lua_cjson'
#
# Task 4 uncomments this block and adds `georgik/lua` to main/idf_component.yml
# in the same commit. Until then, keeping it off makes Tasks 1-3 build
# standalone and fast, which is the point of staging them.
#
# set(EXTRA_COMPONENT_DIRS
#     "${CMAKE_CURRENT_LIST_DIR}/../zhac-components/components"
#     "${CMAKE_CURRENT_LIST_DIR}/../zhac-net-core/components"
#     "${CMAKE_CURRENT_LIST_DIR}/../zhac-main-core/components"
# )

# embedded-zhc is the host-testable C++20 device library; firmwares resolve it
# via EMBEDDED_ZHC_PATH (zhc_adapter's CMakeLists checks this var). Default to
# the sibling repo so a manual `idf.py build` from this dir works.
if(NOT DEFINED ENV{EMBEDDED_ZHC_PATH})
    set(ENV{EMBEDDED_ZHC_PATH} "${CMAKE_CURRENT_LIST_DIR}/../embedded-zhc")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zhac-wired-core)
