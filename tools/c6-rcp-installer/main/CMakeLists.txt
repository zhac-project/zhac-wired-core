# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The C6 app image is embedded, so the tool is one app to flash. It is the
# APP-ONLY esp_ot_rcp.bin from tools/build-rcp.sh's build dir -- never the
# merged zhac-rcp-c6-*.bin (that one starts with the C6 bootloader and is
# rejected at runtime anyway). Override with -D RCP_BIN=/path/to/app.bin.
if(NOT RCP_BIN)
    set(RCP_BIN "${CMAKE_CURRENT_LIST_DIR}/../../../build.rcp-ota/build/esp_ot_rcp.bin")
endif()
get_filename_component(RCP_BIN "${RCP_BIN}" ABSOLUTE)
if(NOT EXISTS "${RCP_BIN}")
    message(FATAL_ERROR "C6 app image not found: ${RCP_BIN}\n"
                        "From the repo root: RCP_EXTRA_DEFAULTS=$PWD/tools/c6-rcp-installer/rcp-ota.defaults tools/build-rcp.sh build.rcp-ota/merged-unused.bin build.rcp-ota")
endif()
file(SHA256 "${RCP_BIN}" RCP_SHA256)
file(SIZE "${RCP_BIN}" RCP_SIZE)
message(STATUS "c6-rcp-installer: embedding ${RCP_BIN} (${RCP_SIZE} B, sha256 ${RCP_SHA256})")

# Copied under a fixed name so the embed symbol does not depend on RCP_BIN.
configure_file("${RCP_BIN}" "${CMAKE_CURRENT_BINARY_DIR}/rcp_app.bin" COPYONLY)

idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_hosted esp_driver_uart esp_driver_gpio
                                     esp_app_format bootloader_support esp_event mbedtls
                       EMBED_FILES "${CMAKE_CURRENT_BINARY_DIR}/rcp_app.bin")
target_compile_definitions(${COMPONENT_LIB} PRIVATE RCP_SHA256_HEX="${RCP_SHA256}")
