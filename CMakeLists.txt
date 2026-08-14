# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# zhac-wired-core — single-chip ZHAC firmware (ESP32-P4 + wired Ethernet).
# Sibling repos are read-only inputs; the bridge shim under components/
# overrides hap_master and hap_slave so the SPI-bridge symbols become
# local function calls. Local components/ wins by ESP-IDF's "first match
# by name" rule.
cmake_minimum_required(VERSION 3.22)

# Shared components. This mirrors zhac-net-core's resolution, including its
# three-way fallback, and the scope is deliberate: ONLY zhac-components.
#
# Do NOT add zhac-main-core/components or zhac-mono-core/components here.
# ESP-IDF *builds* only what is reachable from main's REQUIRES, but it
# *resolves requirements* for everything it discovers, so a wider search path
# drags in dependencies this firmware does not have. Adding zhac-main-core
# fails configure outright:
#
#   Failed to resolve component 'lua' required by component 'lua_cjson'
#
# zhac-components itself is safe to scan: it contains no lua_* components, and
# the only member declaring a registry dependency is arduinojson (see
# main/idf_component.yml for why that one must not be path-added).
#
# ws_server and remote_client live in zhac-net-core and are pulled by explicit
# path in main/idf_component.yml rather than by widening this search path.
if(DEFINED ENV{IDF_COMPONENT_OVERRIDE_PATH})
    set(EXTRA_COMPONENT_DIRS "$ENV{IDF_COMPONENT_OVERRIDE_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../zhac-components/components")
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../zhac-components/components")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../components")
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
else()
    message(FATAL_ERROR
        "shared components dir not found — set IDF_COMPONENT_OVERRIDE_PATH or "
        "clone zhac-components as a sibling")
endif()

# embedded-zhc is the host-testable C++20 device library; firmwares resolve it
# via EMBEDDED_ZHC_PATH (zhc_adapter's CMakeLists checks this var). Default to
# the sibling repo so a manual `idf.py build` from this dir works.
if(NOT DEFINED ENV{EMBEDDED_ZHC_PATH})
    set(ENV{EMBEDDED_ZHC_PATH} "${CMAKE_CURRENT_LIST_DIR}/../embedded-zhc")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zhac-wired-core)

# SPA -> "spa" SPIFFS partition image, built from the sibling www-spa output.
# Run `npm run build` in www-spa FIRST: `idf.py build` never runs it, so a
# stale or empty partition is otherwise silent. FLASH_IN_PROJECT makes
# `idf.py flash` write it. Guarded so the firmware still builds with an empty
# spa partition when dist/ is absent.
#
# Unlike zhac-net-core this repo has NO www-spa submodule; it consumes the
# sibling checkout directly. Do not add a submodule to "match" net-core -- a
# second checkout is exactly what causes the stale-SPIFFS trap there.
set(ZHAC_SPA_DIST "${CMAKE_CURRENT_LIST_DIR}/../www-spa/dist")
if(EXISTS "${ZHAC_SPA_DIST}/index.html")
    spiffs_create_partition_image(spa "${ZHAC_SPA_DIST}" FLASH_IN_PROJECT)
    message(STATUS "ZHAC: SPA image <- ${ZHAC_SPA_DIST}")
else()
    message(WARNING "ZHAC: ${ZHAC_SPA_DIST}/index.html not found — 'spa' partition "
                    "will be empty. Build www-spa (npm run build), then reflash.")
endif()
