# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for esp_zigbee_backend's pure ZCL frame builders.
#
# Deliberately depends on nothing but the header under test -- no ESP-IDF, no
# FreeRTOS, no radio. That is the point: the byte layouts can be verified on
# any machine, without the hardware that everything else in this backend is
# gated on.
#
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
#
# NOTE: run with a clean PATH if ESP-IDF is active in the shell -- the
# ESP32-ULP toolchain's `as` shadows the host assembler and breaks g++:
#   export PATH=/usr/bin:$PATH
cmake_minimum_required(VERSION 3.16)
project(esp_zigbee_backend_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

add_executable(zcl_frame_tests test_zcl_frame.cpp)
add_test(NAME zcl_frame COMMAND zcl_frame_tests)
