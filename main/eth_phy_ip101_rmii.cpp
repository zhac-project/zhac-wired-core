// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// eth_phy_ip101_rmii.cpp -- board_eth_new() for the ESP32-P4 SKU.
//
// Board: Guition JC-ESP32P4-M3-DEV. IP101 PHY at SMI address 1, over RMII,
// wired to ESP-IDF's default esp32p4 EMAC pins.
//
// Compiled only when IDF_TARGET is esp32p4; see main/CMakeLists.txt.
#include "board_eth.h"

#include "driver/gpio.h"
#include "esp_check.h"      // ESP_RETURN_ON_ERROR
#include "esp_eth_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "eth.ip101";

esp_err_t board_eth_new(esp_eth_mac_t** out_mac, esp_eth_phy_t** out_phy) {
    if (!out_mac || !out_phy) return ESP_ERR_INVALID_ARG;
    *out_mac = nullptr;
    *out_phy = nullptr;

    // Power the PHY before anything touches SMI. Without this the MDIO reads
    // during PHY detection come back all-ones and probing fails with a
    // misleading "no PHY found".
#if CONFIG_ZHAC_ETH_PHY_PWR_GPIO >= 0
    gpio_config_t pwr{};
    pwr.pin_bit_mask = 1ULL << CONFIG_ZHAC_ETH_PHY_PWR_GPIO;
    pwr.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&pwr), TAG, "PHY power gpio_config failed");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_ZHAC_ETH_PHY_PWR_GPIO), 1),
        TAG, "PHY power set_level failed");
    vTaskDelay(pdMS_TO_TICKS(10));   // let the rail and the PHY's POR settle
#endif

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();

    // Every field is assigned explicitly rather than starting from
    // ETH_ESP32_EMAC_DEFAULT_CONFIG(). That macro is written for C: under C++
    // with this project's -Wextra -Werror it trips -Wmissing-field-initializers
    // on the fields it omits, and its designators are not in declaration order,
    // which C++20 rejects outright. Spelling the pins out also documents the
    // board, which is worth more here than macro reuse.
    //
    // Values are the Guition wiring, which happens to match ESP-IDF's esp32p4
    // defaults exactly.
    eth_esp32_emac_config_t emac_cfg = {};
    emac_cfg.smi_gpio.mdc_num  = CONFIG_ZHAC_ETH_MDC_GPIO;    // 31
    emac_cfg.smi_gpio.mdio_num = CONFIG_ZHAC_ETH_MDIO_GPIO;   // 52
    emac_cfg.interface         = EMAC_DATA_INTERFACE_RMII;
    // Clock comes from the PHY. Do NOT switch this to EMAC_CLK_OUT: with PSRAM
    // enabled both share the MPLL, and at 80 MHz PSRAM speed no integer divisor
    // yields 50 MHz within tolerance -- EMAC init fails outright.
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = CONFIG_ZHAC_ETH_REF_CLK_GPIO;  // 50
    // RMII data plane. Each signal is IO_MUX-fixed to its own small candidate
    // set; these are the P4 defaults and this board follows them.
    emac_cfg.emac_dataif_gpio.rmii.crs_dv_num = 28;
    emac_cfg.emac_dataif_gpio.rmii.rxd0_num   = 29;
    emac_cfg.emac_dataif_gpio.rmii.rxd1_num   = 30;
    emac_cfg.emac_dataif_gpio.rmii.txd0_num   = 34;
    emac_cfg.emac_dataif_gpio.rmii.txd1_num   = 35;
    emac_cfg.emac_dataif_gpio.rmii.tx_en_num  = 49;
    // Only meaningful when clock_mode is EMAC_CLK_OUT (looped back externally);
    // disabled here to match EMAC_CLK_EXT_IN above.
    emac_cfg.clock_config_out_in.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config_out_in.rmii.clock_gpio = -1;
    emac_cfg.dma_burst_len = ETH_DMA_BURST_LEN_32;
    emac_cfg.intr_priority = 0;   // let the driver choose
    emac_cfg.mdc_freq_hz   = 0;   // driver default (CSR clock up to 2.5 MHz)

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 returned null");
        return ESP_FAIL;
    }

    // Generic 802.3 driver, not esp_eth_phy_new_ip101(): IDF v6.0 moved the
    // chip-specific PHY drivers out to the esp-eth-drivers registry component,
    // and the generic driver covers any IEEE 802.3-compliant PHY. If the IP101
    // later needs post-init tuning, add it via esp_eth_ioctl() here rather than
    // taking on the extra dependency.
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = CONFIG_ZHAC_ETH_PHY_ADDR;   // 1 on this board
    phy_cfg.reset_gpio_num = -1;                         // no separate nRST line
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_generic returned null");
        mac->del(mac);
        return ESP_FAIL;
    }

    *out_mac = mac;
    *out_phy = phy;
    return ESP_OK;
}

esp_err_t board_eth_post_install(esp_eth_handle_t handle) {
    // The IP101 needs nothing after driver install. Note IDF's own
    // ethernet/basic example calls this board's GPIO51 the PHY *reset* line
    // rather than a power enable; holding it high above is equivalent for our
    // purposes, and is what has been built and tested here.
    (void)handle;
    return ESP_OK;
}
