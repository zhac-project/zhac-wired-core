# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# esp_zigbee_backend -- Phase 1. The radio layer for this SKU.
#
# Serves BOTH targets from one implementation; only the radio_mode differs and
# the lib's own Kconfig picks it per target:
#
#   esp32s31 -> CONFIG_ZB_RADIO_NATIVE       (SOC_IEEE802154_SUPPORTED)
#   esp32p4  -> CONFIG_ZB_RADIO_SPINEL_UART  (external C6 running ot_rcp)
#
# ⚠ REGISTRATION IS UNCONDITIONAL, AND MUST STAY THAT WAY.
#
# Do not wrap idf_component_register() in `if(CONFIG_...)`. ESP-IDF processes
# component CMakeLists TWICE, and in the first "early expansion" pass -- the one
# that resolves the dependency graph -- CONFIG_* symbols are NOT yet defined.
# An earlier version gated on CONFIG_ZHAC_ESP_ZIGBEE and silently took the
# disabled branch during that pass, registering with an empty REQUIRES list.
# The component then compiled with no include paths and failed on the first
# #include, while `idf.py menuconfig` showed the option correctly enabled.
#
# Compile-time gating belongs in the SOURCE (#if CONFIG_ZHAC_ESP_ZIGBEE), where
# the symbol genuinely exists. Same rule applies to zigbee_mgr/radio_stubs.cpp.
# Gating on IDF_TARGET is fine -- that IS available early (see tg_gw_stub).
idf_component_register(
    SRCS "esp_zigbee_backend.cpp" "esp_zb_interview.cpp" "esp_zb_configure.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        espressif__esp-zigbee-lib
        device_backend
        zhc_adapter
        zap_common
        zap_store
        zigbee_mgr        # the local override: device pool + diagnostics
        event_bus
        device_shadow
        nvs_flash
        esp_timer
        freertos
)
