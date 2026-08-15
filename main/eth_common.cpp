// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// eth_common.cpp -- target-neutral Ethernet plumbing: esp_netif, the DHCP
// client, link/IP event tracking and the status snapshot.
//
// Everything board-specific (MAC/PHY creation, data interface, pin assignment,
// PHY power sequencing) lives behind board_eth_new() -- see board_eth.h. This
// file must stay free of #if IDF_TARGET.
//
// Replaces mono-core's wifi.cpp and is deliberately much smaller: there are no
// credentials, no AP fallback, no scan and no provisioning. The link is either
// up or it is not, and recovery is the PHY's autonegotiation plus esp_eth's own
// link-check timer -- no retry loop needed here.
#include "eth.h"

#include "board_eth.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

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

    esp_eth_mac_t* mac = nullptr;
    esp_eth_phy_t* phy = nullptr;
    esp_err_t err = board_eth_new(&mac, &phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board_eth_new failed: %s -- continuing with link down",
                 esp_err_to_name(err));
        return;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_cfg, &s_handle);
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
            // ETH_SPEED_1000M exists on targets with a gigabit MAC (S31);
            // report it rather than silently collapsing to 10.
            switch (spd) {
                case ETH_SPEED_100M: out->speed_mbps = 100;  break;
                case ETH_SPEED_10M:  out->speed_mbps = 10;   break;
                default:             out->speed_mbps = 1000; break;
            }
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
