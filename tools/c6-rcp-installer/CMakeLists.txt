# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# One-off ESP32-P4 tool: installs ot_rcp on the Guition JC-ESP32P4-M3's
# on-module ESP32-C6 over ESP-Hosted slave OTA (SDIO). See README.md.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_rcp_installer)
