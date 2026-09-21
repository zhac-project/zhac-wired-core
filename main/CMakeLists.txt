# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Two components are conspicuously absent from REQUIRES and must stay absent:
#
#   esp_wifi     -- this SKU is Ethernet-only. tools/check_no_rainmaker.sh
#                   makes that a build failure rather than a convention.
#   znp_driver / zigbee_mgr / zigbee_backend / ezsp_*
#                -- no radio in Phase 0. Phase 1 brings exactly one backend,
#                   esp_zigbee_backend over ot_rcp. The protocol-neutral
#                   device pool comes from components/zigbee_mgr instead;
#                   see that component's CMakeLists for why.
# Ethernet is split into target-neutral plumbing (eth_common.cpp) plus exactly
# one board/PHY implementation of board_eth_new() -- see main/board_eth.h.
# Selecting it here rather than with #if inside a shared file means a build for
# one target cannot even compile the other's code.
if(IDF_TARGET STREQUAL "esp32p4")
    set(BOARD_ETH_SRC "eth_phy_ip101_rmii.cpp")   # Guition JC-ESP32P4-M3-DEV
elseif(IDF_TARGET STREQUAL "esp32s31")
    set(BOARD_ETH_SRC "eth_phy_yt8531_rgmii.cpp") # S31 Function-Core
else()
    message(FATAL_ERROR
        "zhac-wired-core: no Ethernet PHY implementation for IDF_TARGET "
        "'${IDF_TARGET}'. Add main/eth_phy_<phy>_<iface>.cpp implementing "
        "board_eth_new() (see main/board_eth.h) and a branch here, plus a "
        "sdkconfig.defaults.${IDF_TARGET}.")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "eth_common.cpp"
        "${BOARD_ETH_SRC}"
        "net_discovery.cpp"
        "sys_state.cpp"
        "sys_diag.cpp"
        "log_ring.cpp"
        "device_options.cpp"
        "groups_store.cpp"
        "dgm_store.cpp"
        "spa_serve.cpp"
        "metrics_mqtt.cpp"
        "radio_state.cpp"
        "ws_bridge.cpp"
        "mqtt_glue.cpp"
        "ota_update.cpp"
        "auth.cpp"
        "api_net.cpp"
        "api_status.cpp"
        "api_devices.cpp"
        "api_groups.cpp"
        "api_remote.cpp"
        "api_rules.cpp"
        "api_scripts.cpp"
        "api_system.cpp"
        "status_led.cpp"
    INCLUDE_DIRS "."
    # Moves zhc_adapter's ~105 KB of lookup-table .bss to PSRAM. Without it the
    # esp32s31 build has too little internal DRAM left for EMAC DMA descriptors
    # and Ethernet dies at init. See the file for the full measurement.
    LDFRAGMENTS "psram_bss.lf"
    REQUIRES
        # -- IDF ----------------------------------------------------------
        esp_system
        esp_psram
        esp_timer
        esp_app_format
        nvs_flash
        spi_flash
        spiffs
        esp_eth
        esp_event
        esp_netif
        esp_http_server
        # OTA (ota_update.cpp): HTTPS download into the idle slot.
        esp_https_ota
        esp_http_client
        app_update
        # auth.cpp: PSA hashing for the admin password; getpeername for lockout.
        mbedtls
        lwip
        # IDF v6.0 split the monolithic `driver` component; `driver/gpio.h`
        # now comes from esp_driver_gpio. Requiring `driver` builds but the
        # header is not on the include path.
        esp_driver_gpio
        mdns
        # -- local components/ --------------------------------------------
        hap_master
        hap_slave
        mono_bridge
        lua_engine
        lua_cjson
        zigbee_mgr
        esp_zigbee_backend
        # Empty on p4/s3 where the sibling tg_gw supplies real sources; carries
        # the stubs on targets it does not cover. See its CMakeLists.
        tg_gw_stub
        # Must be required EXPLICITLY. With set(COMPONENTS main) a component
        # that nothing requires is not built -- and a component that is not
        # built contributes no Kconfig, so CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        # in sdkconfig.defaults became an "unknown symbol" and was silently
        # ignored, disabling coredumps. tools/check_resolved_config.sh now
        # asserts it.
        espcoredump
        # -- zhac-components ----------------------------------------------
        hap_protocol
        zap_common
        event_bus
        metrics
        zap_store
        device_shadow
        device_backend
        zhc_adapter
        device_cmd
        led_strip
        simple_rules
        rule_store
        cron_parser
        mqtt_gw
        ntp_cfg
        ha_bridge
        mqtt
        arduinojson
        # -- zhac-net-core ------------------------------------------------
        ws_server
        remote_client
)

# ── Web UI, embedded ────────────────────────────────────────────────────────
# tools/pack_spa.py gzips ../www-spa/dist into spa.pack; spa_serve.cpp serves it
# from flash. Rebuilt whenever index.html changes (Vite renames every changed
# asset, which changes index.html).
set(ZHAC_SPA_DIST "${CMAKE_CURRENT_LIST_DIR}/../../www-spa/dist")
set(ZHAC_SPA_PACK "${CMAKE_BINARY_DIR}/spa.pack")
set(ZHAC_SPA_DEPS "${CMAKE_CURRENT_LIST_DIR}/../tools/pack_spa.py")
if(EXISTS "${ZHAC_SPA_DIST}/index.html")
    list(APPEND ZHAC_SPA_DEPS "${ZHAC_SPA_DIST}/index.html")
else()
    message(WARNING "ZHAC: ${ZHAC_SPA_DIST}/index.html not found -- building without "
                    "the web UI. Run `npm ci && npm run build` in www-spa first.")
endif()
idf_build_get_property(zhac_python PYTHON)
add_custom_command(
    OUTPUT "${ZHAC_SPA_PACK}"
    COMMAND "${zhac_python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/pack_spa.py"
            "${ZHAC_SPA_DIST}" "${ZHAC_SPA_PACK}"
    DEPENDS ${ZHAC_SPA_DEPS}
    VERBATIM)
add_custom_target(zhac_spa_pack DEPENDS "${ZHAC_SPA_PACK}")
add_dependencies(${COMPONENT_LIB} zhac_spa_pack)
target_add_binary_data(${COMPONENT_LIB} "${ZHAC_SPA_PACK}" BINARY)
