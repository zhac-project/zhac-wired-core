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
        "log_ring.cpp"
        "device_options.cpp"
        "groups_store.cpp"
        "spa_serve.cpp"
        "metrics_mqtt.cpp"
        "radio_state.cpp"
        "ws_bridge.cpp"
        "api_net.cpp"
        "api_status.cpp"
        "api_devices.cpp"
        "api_groups.cpp"
        "api_remote.cpp"
        "api_rules.cpp"
        "api_scripts.cpp"
        "api_system.cpp"
    INCLUDE_DIRS "."
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
        # Empty on p4/s3 where the sibling tg_gw supplies real sources; carries
        # the stubs on targets it does not cover. See its CMakeLists.
        tg_gw_stub
        # -- zhac-components ----------------------------------------------
        hap_protocol
        zap_common
        event_bus
        metrics
        zap_store
        device_shadow
        device_backend
        zhc_adapter
        simple_rules
        rule_store
        cron_parser
        mqtt_gw
        mqtt
        arduinojson
        # -- zhac-net-core ------------------------------------------------
        ws_server
        remote_client
)
