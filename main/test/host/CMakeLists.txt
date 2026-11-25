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

# json_buf.h (Task 3) includes utf8_seq.h from zhac-components' ws_server (ZHAC_COMPONENTS above).

add_executable(log_cloud_core_tests
    test_log_cloud_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../log_cloud_core.cpp)
target_include_directories(log_cloud_core_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${ZHAC_COMPONENTS}/components/ws_server/include)
add_test(NAME log_cloud_core COMMAND log_cloud_core_tests)

add_executable(req_filter_tests test_req_filter.cpp)
target_include_directories(req_filter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME req_filter COMMAND req_filter_tests)

# Hub pin configuration: PUT /api/io validation, defaults, lookups (spec 2026-11-15 GPIO control).
add_executable(user_io_config_tests test_user_io_config.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../user_io_config.cpp)
target_include_directories(user_io_config_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    "${ZHAC_COMPONENTS}/components/arduinojson/src")
add_test(NAME user_io_config COMMAND user_io_config_tests)

# Alarm panel: the state machine, the keypad code policy and the config JSON
# (spec 2026-11-24 security mode).
add_executable(alarm_core_tests test_alarm_core.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../alarm_core.cpp)
target_include_directories(alarm_core_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    "${ZHAC_COMPONENTS}/components/arduinojson/src"
    "${ZHAC_COMPONENTS}/components/ws_server/include")
add_test(NAME alarm_core COMMAND alarm_core_tests)

# Hub sounds: the built-in sound table and the synthesizer (spec 2026-11-25 hub sounds).
add_executable(sound_synth_tests test_sound_synth.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../sound_synth.cpp)
target_include_directories(sound_synth_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    "${ZHAC_COMPONENTS}/components/arduinojson/src"
    "${ZHAC_COMPONENTS}/components/ws_server/include")
add_test(NAME sound_synth COMMAND sound_synth_tests)
