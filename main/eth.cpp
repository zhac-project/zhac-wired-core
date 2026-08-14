// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// eth.cpp -- internal EMAC + external RMII PHY, DHCP client.
//
// Replaces mono-core's wifi.cpp. Deliberately much smaller: there are no
// credentials, no AP fallback, no scan and no provisioning. The link is
// either up or it is not, and recovery is the PHY's autonegotiation plus
// esp_eth's own link-check timer -- no retry loop needed here.
//
// Board: Guition JC-ESP32P4-M3-DEV, IP101 PHY at SMI address 1, wired to
// ESP-IDF's default esp32p4 EMAC pins, so only the control-plane pins and
// the PHY power enable are configured here.
#include "eth.h"

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "eth";

static esp_netif_t*     s_netif   = nullptr;
static esp_eth_handle_t s_handle  = nullptr;
static bool             s_link_up = false;
static bool             s_has_ip  = false;

static void on_eth_event(void*, esp_event_base_t, int32_t id, void*) {
    switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            s_link_up = true;
            ESP_LOGI(TAG, "link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            s_link_up = false;
            s_has_ip  = false;
            ESP_LOGW(TAG, "link down");
            break;
        case ETHERNET_EVENT_STOP:
            s_link_up = false;
            s_has_ip  = false;
            break;
        default:
            break;
    }
}

static void on_got_ip(void*, esp_event_base_t, int32_t, void* data) {
    const auto* ev = static_cast<ip_event_got_ip_t*>(data);
    s_has_ip = true;
    ESP_LOGI(TAG, "got IP " IPSTR " gw " IPSTR,
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
}

void eth_start() {
    if (s_handle) return;   // idempotent

    ESP_ERROR_CHECK(esp_netif_init());
    // May already exist if another subsystem created it first; tolerate that.
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);

    // Power the PHY before anything touches SMI. Without this the MDIO reads
    // during PHY detection come back all-ones and probing fails with a
    // misleading "no PHY found".
#if CONFIG_ZHAC_ETH_PHY_PWR_GPIO >= 0
    gpio_config_t pwr{};
    pwr.pin_bit_mask = 1ULL << CONFIG_ZHAC_ETH_PHY_PWR_GPIO;
    pwr.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&pwr));
    ESP_ERROR_CHECK(gpio_set_level(
        static_cast<gpio_num_t>(CONFIG_ZHAC_ETH_PHY_PWR_GPIO), 1));
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
    // Values are the Guition JC-ESP32P4-M3-DEV wiring, which happens to match
    // ESP-IDF's esp32p4 defaults exactly.
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
        ESP_LOGE(TAG, "EMAC init failed -- continuing with link down");
        return;
    }

    // Generic 802.3 driver, not esp_eth_phy_new_ip101(): IDF v6.0 moved the
    // chip-specific PHY drivers out to the esp-eth-drivers registry component,
    // and the generic driver covers any IEEE 802.3-compliant PHY. If the IP101
    // later needs post-init tuning, add it via esp_eth_ioctl() rather than
    // taking on the extra dependency.
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = CONFIG_ZHAC_ETH_PHY_ADDR;   // 1 on this board
    phy_cfg.reset_gpio_num = -1;                         // no separate nRST line
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "PHY init failed -- continuing with link down");
        return;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s -- link stays down",
                 esp_err_to_name(err));
        s_handle = nullptr;
        return;
    }

    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &on_eth_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &on_got_ip, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(s_handle));
    ESP_LOGI(TAG, "started -- waiting for link + DHCP");
}

bool eth_link_up() { return s_link_up; }

void eth_get_status(NetStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->link_up = s_link_up;
    out->has_ip  = s_has_ip;

    uint8_t mac[6]{};
    if (s_handle &&
        esp_eth_ioctl(s_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
        snprintf(out->mac, sizeof(out->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (s_link_up && s_handle) {
        eth_speed_t  spd = ETH_SPEED_10M;
        eth_duplex_t dup = ETH_DUPLEX_HALF;
        if (esp_eth_ioctl(s_handle, ETH_CMD_G_SPEED, &spd) == ESP_OK) {
            out->speed_mbps = (spd == ETH_SPEED_100M) ? 100 : 10;
        }
        if (esp_eth_ioctl(s_handle, ETH_CMD_G_DUPLEX_MODE, &dup) == ESP_OK) {
            out->duplex_full = (dup == ETH_DUPLEX_FULL);
        }
    }

    if (s_has_ip && s_netif) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
            snprintf(out->ip,      sizeof(out->ip),      IPSTR, IP2STR(&ip.ip));
            snprintf(out->netmask, sizeof(out->netmask), IPSTR, IP2STR(&ip.netmask));
            snprintf(out->gw,      sizeof(out->gw),      IPSTR, IP2STR(&ip.gw));
        }
    }
}
