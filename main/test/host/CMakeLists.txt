# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for pure helpers in main/ (no ESP-IDF, no FreeRTOS).
#
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
#
# NOTE: run with a clean PATH if ESP-IDF is active in the shell -- the
# ESP32-ULP toolchain's `as` shadows the host assembler and breaks g++:
#   export PATH=/usr/bin:$PATH
cmake_minimum_required(VERSION 3.16)
project(wired_main_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

# The round-trip test parses pages with the same ArduinoJson the firmware
# vendors (header-only), from the sibling zhac-components checkout.
set(ZHAC_COMPONENTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../zhac-components"
    CACHE PATH "zhac-components checkout")
if(NOT EXISTS "${ZHAC_COMPONENTS}/components/arduinojson/src/ArduinoJson.h")
    message(FATAL_ERROR "ArduinoJson not found; pass -DZHAC_COMPONENTS=<zhac-components checkout>")
endif()

add_executable(devlist_page_tests test_devlist_page.cpp)
target_include_directories(devlist_page_tests PRIVATE
    "${ZHAC_COMPONENTS}/components/arduinojson/src")
add_test(NAME devlist_page COMMAND devlist_page_tests)
