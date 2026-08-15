// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// eth_phy_yt8531_rgmii.cpp -- board_eth_new() + post-install quirks for the
// ESP32-S31 SKU.
//
// Board: ESP32-S31 Function-Core. Motorcomm YT8531 PHY over **RGMII** (the P4
// SKU is RMII/IP101), driven by the S31's gigabit EMAC. Pin values are IDF's
// esp32s31 defaults, which this board follows.
//
// Compiled only when IDF_TARGET is esp32s31; see main/CMakeLists.txt.
#include "board_eth.h"

#include "esp_check.h"
#include "esp_eth_driver.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "eth.yt8531";

esp_err_t board_eth_new(esp_eth_mac_t** out_mac, esp_eth_phy_t** out_phy) {
    if (!out_mac || !out_phy) return ESP_ERR_INVALID_ARG;
    *out_mac = nullptr;
    *out_phy = nullptr;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();

    // Fields assigned explicitly rather than via ETH_ESP32_EMAC_DEFAULT_CONFIG():
    // that macro is C and trips -Wmissing-field-initializers plus C++20's
    // designator-order rule under this project's -Wextra -Werror. Same reasoning
    // as the P4 file; see eth_phy_ip101_rmii.cpp.
    eth_esp32_emac_config_t emac_cfg = {};
    emac_cfg.smi_gpio.mdc_num  = CONFIG_ZHAC_ETH_MDC_GPIO;    // 5
    emac_cfg.smi_gpio.mdio_num = CONFIG_ZHAC_ETH_MDIO_GPIO;   // 6
    emac_cfg.interface         = EMAC_DATA_INTERFACE_RGMII;

    // RGMII clocking. Unlike RMII there is no single REF_CLK: the MAC takes a
    // receive clock from the PHY and drives its own transmit clock.
    // clock_phy_ref_gpio = -1 -> we do not feed the PHY a reference clock; the
    // Function-Core board's PHY has its own 25 MHz crystal.
    emac_cfg.clock_config.rgmii.clock_rx_gpio      = 14;
    emac_cfg.clock_config.rgmii.clock_tx_gpio      = 13;
    emac_cfg.clock_config.rgmii.clock_phy_ref_gpio = -1;

    // RGMII data plane: 4 bits each way plus control, against RMII's 2+1.
    emac_cfg.emac_dataif_gpio.rgmii.tx_ctl_num = 12;
    emac_cfg.emac_dataif_gpio.rgmii.txd0_num   = 8;
    emac_cfg.emac_dataif_gpio.rgmii.txd1_num   = 9;
    emac_cfg.emac_dataif_gpio.rgmii.txd2_num   = 10;
    emac_cfg.emac_dataif_gpio.rgmii.txd3_num   = 11;
    emac_cfg.emac_dataif_gpio.rgmii.rx_ctl_num = 15;
    emac_cfg.emac_dataif_gpio.rgmii.rxd0_num   = 19;
    emac_cfg.emac_dataif_gpio.rgmii.rxd1_num   = 18;
    emac_cfg.emac_dataif_gpio.rgmii.rxd2_num   = 17;
    emac_cfg.emac_dataif_gpio.rgmii.rxd3_num   = 16;

    emac_cfg.clock_config_out_in.rgmii.clock_rx_gpio      = -1;
    emac_cfg.clock_config_out_in.rgmii.clock_tx_gpio      = -1;
    emac_cfg.clock_config_out_in.rgmii.clock_phy_ref_gpio = -1;

    // 16, not the P4's 32 -- IDF's esp32s31 default.
    emac_cfg.dma_burst_len = ETH_DMA_BURST_LEN_16;
    emac_cfg.intr_priority = 0;
    emac_cfg.mdc_freq_hz   = 0;

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 returned null");
        return ESP_FAIL;
    }

    // Generic 802.3 driver: IDF ships no dedicated YT8531 driver, and its own
    // ethernet/basic example drives this exact PHY generically, doing the
    // chip-specific work through esp_eth_ioctl() -- which we mirror in
    // board_eth_post_install() below.
    //
    // Unlike the P4 board (where the equivalent pin is held high as a power
    // enable), the reset line is handed to the PHY driver so it performs a
    // proper reset pulse before probing.
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = CONFIG_ZHAC_ETH_PHY_ADDR;      // -1 = autodetect
    phy_cfg.reset_gpio_num = CONFIG_ZHAC_ETH_PHY_RST_GPIO;  // 7
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

// YT8531 post-reset fixups. Both are mandatory for a usable RGMII link and
// neither can be expressed in eth_phy_config_t, which is why board_eth.h has a
// post-install hook at all. Sequence mirrors IDF's ethernet/basic example
// (copy kept at extra/s31-spike/reference/yt8531_init_reference.c).
esp_err_t board_eth_post_install(esp_eth_handle_t handle) {
    // 1. The YT8531 DISABLES auto-negotiation when the generic 802.3 driver
    //    resets it -- undocumented, observed behaviour. Turn it back on or the
    //    link never negotiates speed/duplex.
    bool auto_nego_en = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_S_AUTONEGO, &auto_nego_en),
                        TAG, "re-enabling auto-negotiation failed");

    // 2. RGMII needs ~2 ns of clock delay on BOTH paths, set through the PHY's
    //    extended register interface: write the ext-register address to 0x1E,
    //    then read/modify/write the data through 0x1F.
    uint32_t reg_val = 0;
    esp_eth_phy_reg_rw_data_t phy_reg = {};
    phy_reg.reg_value_p = &reg_val;

    // RX ~2 ns coarse delay -- EXT_CHIP_CONFIG (0xA001), bit 8 = rxc_dly_en.
    reg_val = 0xA001;
    phy_reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &phy_reg),
                        TAG, "select EXT Chip_Config failed");
    phy_reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &phy_reg),
                        TAG, "read Chip_Config failed");
    reg_val |= (1U << 8);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &phy_reg),
                        TAG, "write Chip_Config failed");

    // TX ~2 ns delay -- EXT_RGMII_CONFIG1 (0xA003). Clear tx_delay_sel [3:0]
    // and tx_delay_sel_fe [7:4], then set both to 13 steps x 150 ps ~= 1.95 ns.
    reg_val = 0xA003;
    phy_reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &phy_reg),
                        TAG, "select EXT RGMII_Config1 failed");
    phy_reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &phy_reg),
                        TAG, "read RGMII_Config1 failed");
    reg_val = (reg_val & ~0x00FFU) | (13U << 4) | (13U << 0);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &phy_reg),
                        TAG, "write RGMII_Config1 failed");

    ESP_LOGI(TAG, "YT8531 ready: autoneg re-enabled, RGMII delays ~2 ns Rx / ~2 ns Tx");
    return ESP_OK;
}
