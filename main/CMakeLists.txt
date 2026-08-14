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
#                   device pool comes from components/zigbee_pool instead;
#                   see that component's CMakeLists for why.
idf_component_register(
    SRCS
        "main.cpp"
        "eth.cpp"
        "net_discovery.cpp"
        "sys_state.cpp"
        "log_ring.cpp"
        "device_options.cpp"
        "groups_store.cpp"
        "spa_serve.cpp"
        "metrics_mqtt.cpp"
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
        zigbee_pool
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
