# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Tasks 1-2: board bring-up plus the Ethernet netif. No sibling components in
# the graph yet -- the goal is to prove the toolchain, PSRAM detection, the
# partition table and the EMAC/PHY driver before pulling ZHAC's stack in.
#
# Note esp_wifi is deliberately absent and must stay absent.
idf_component_register(
    SRCS
        "main.cpp"
        "eth.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_system
        esp_psram
        esp_timer
        nvs_flash
        spi_flash
        esp_eth
        esp_event
        esp_netif
        # IDF v6.0 split the monolithic `driver` component; `driver/gpio.h`
        # now comes from esp_driver_gpio. Requiring `driver` builds but the
        # header is not on the include path.
        esp_driver_gpio
)
