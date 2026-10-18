// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// wifi_hosted.cpp -- Wi-Fi station for the P4 through an ESP-Hosted
// co-processor (an ESP32-C5 on SPI; pins in sdkconfig.defaults.wifi-hosted).
//
// esp_wifi_remote turns every esp_wifi_* call into an RPC to the co-processor,
// and the data path is an ordinary esp_netif. So DHCP, mDNS (its predefined
// WIFI_STA_DEF interface), the cloud client (it already checks WIFI_STA_DEF)
// and the web server need no Wi-Fi code of their own -- this file only joins
// the network and keeps it joined.
//
// Credentials live in the P4's NVS (namespace "wifi_sta"), not the
// co-processor's: swap the C5 and the hub still knows its network.
#include "wifi_hosted.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ntp_cfg.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "wifi";
static const char* kNvsNs = "wifi_sta";

// Ethernet's default is 50; anything lower keeps the cable as the default
// route whenever both are up.
static constexpr int kRoutePrio = 20;
static constexpr uint32_t kRetryMs = 5000;
static constexpr EventBits_t kRetryBit = BIT0;

static esp_netif_t*       s_netif = nullptr;
static EventGroupHandle_t s_evt   = nullptr;

// Written by the event handlers, the Wi-Fi task and wifi_hosted_connect();
// read anywhere without a lock -- same contract as eth_common's link flags.
static volatile bool s_ready     = false;
static volatile bool s_connected = false;
static volatile bool s_warned    = false;   // one log line per failure streak
static char s_ssid[33] = {};
static char s_pass[65] = {};

static void load_creds() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;   // never configured
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = '\0';
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = '\0';
    nvs_close(h);
}

static bool save_creds() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, "ssid", s_ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", s_pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static void apply_creds() {
    wifi_config_t wc{};
    // memcpy, not strcpy: a 32-byte SSID fills the field with no NUL, which
    // is how esp_wifi expects it.
    std::memcpy(wc.sta.ssid, s_ssid, std::strlen(s_ssid));
    std::memcpy(wc.sta.password, s_pass, std::strlen(s_pass));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    const esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(e));
}

static void on_wifi(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        if (s_ssid[0] && (s_connected || !s_warned)) {
            ESP_LOGW(TAG, "%s \"%s\" (reason %d) -- retrying every %u s",
                     s_connected ? "lost" : "cannot join", s_ssid, ev->reason,
                     (unsigned)(kRetryMs / 1000));
            s_warned = true;
        }
        s_connected = false;
        // Retried from the task after a pause, not here: an immediate
        // reconnect from the handler spins as fast as the AP refuses.
        if (s_ssid[0]) xEventGroupSetBits(s_evt, kRetryBit);
    }
}

static void on_ip(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        s_connected = true;
        s_warned    = false;
        ESP_LOGI(TAG, "joined \"%s\", got IP " IPSTR " gw " IPSTR, s_ssid,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        // Same reason as eth_common's on_got_ip: every lease rewrites lwIP's
        // time-server list, so the configured servers are put back.
        ntp_cfg_start();
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
    }
}

static void wifi_task(void*) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "co-processor did not answer (esp_wifi_init: %s) -- Wi-Fi off. "
                      "Check the C5 wiring and that it runs the matching ESP-Hosted image.",
                 esp_err_to_name(e));
        vTaskDelete(nullptr);
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // the P4's NVS copy is the one that counts
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (s_ssid[0]) apply_creds();
    e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s -- Wi-Fi off", esp_err_to_name(e));
        vTaskDelete(nullptr);
    }
    s_ready = true;
    if (s_ssid[0]) ESP_LOGI(TAG, "started -- joining \"%s\"", s_ssid);
    else           ESP_LOGI(TAG, "started -- no network yet (web UI: Settings > WiFi)");

    for (;;) {
        xEventGroupWaitBits(s_evt, kRetryBit, pdTRUE, pdFALSE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(kRetryMs));
        if (s_ssid[0] && !s_connected) esp_wifi_connect();
    }
}

void wifi_hosted_start() {
    if (s_evt) return;   // idempotent
    s_evt = xEventGroupCreate();
    load_creds();

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.route_prio = kRoutePrio;
    s_netif = esp_netif_create_wifi(WIFI_IF_STA, &base);
    // Same DHCP hostname as the Ethernet side, so the router lists "zhac".
    esp_netif_set_hostname(s_netif, CONFIG_ZHAC_MDNS_HOSTNAME);
    esp_wifi_set_default_wifi_sta_handlers();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip, nullptr);

    // esp_wifi_init is an RPC that waits for the co-processor, so it runs
    // here and not in app_main.
    if (xTaskCreate(wifi_task, "TaskWifi", 6144, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "TaskWifi create failed -- Wi-Fi off");
    }
}

void wifi_hosted_status(WifiStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->ready      = s_ready;
    out->configured = s_ssid[0] != '\0';
    out->connected  = s_connected;
    std::snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    if (!s_connected) return;
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        std::snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
}

int wifi_hosted_scan(WifiAp* out, size_t max) {
    if (!s_ready || !out || max == 0) return -1;
    if (esp_wifi_scan_start(nullptr, true) != ESP_OK) return -1;
    uint16_t n = static_cast<uint16_t>(max);
    auto* recs = static_cast<wifi_ap_record_t*>(std::calloc(n, sizeof(wifi_ap_record_t)));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    int got = 0;
    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            if (!recs[i].ssid[0]) continue;   // hidden network
            WifiAp& a = out[got++];
            std::memcpy(a.ssid, recs[i].ssid, sizeof(a.ssid) - 1);
            a.ssid[sizeof(a.ssid) - 1] = '\0';
            a.rssi = recs[i].rssi;
            a.open = recs[i].authmode == WIFI_AUTH_OPEN;
        }
    }
    std::free(recs);
    return got;
}

bool wifi_hosted_connect(const char* ssid, const char* pass) {
    if (!ssid || std::strlen(ssid) > 32) return false;
    if (!pass) pass = "";
    if (std::strlen(pass) > 64) return false;
    std::snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    std::snprintf(s_pass, sizeof(s_pass), "%s", pass);
    if (!save_creds()) return false;
    if (!s_ready) return true;   // stored; the task applies it once the co-processor is up
    s_warned = false;
    esp_wifi_disconnect();
    if (!s_ssid[0]) {
        ESP_LOGI(TAG, "network forgotten");
        return true;
    }
    apply_creds();
    esp_wifi_connect();
    ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
    return true;
}
