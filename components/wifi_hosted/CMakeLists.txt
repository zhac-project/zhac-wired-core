# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Wi-Fi station through an ESP-Hosted co-processor (ZHAC_WIFI_HOSTED). The one
# place in this SKU allowed to use esp_wifi -- tools/check_sku_invariants.sh
# exempts this directory and asserts it is absent from the default build graph.
# On the P4, esp_wifi is headers plus netif glue; esp_wifi_remote (from the
# manifest) supplies the API as RPCs to the co-processor.
idf_component_register(SRCS "wifi_hosted.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash ntp_cfg)
