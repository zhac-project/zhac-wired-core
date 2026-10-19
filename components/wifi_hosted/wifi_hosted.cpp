// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// wifi_hosted.cpp -- Wi-Fi station for the P4 or the S31 through an ESP-Hosted
// co-processor (an ESP32-C5 on SPI; pins in sdkconfig.defaults.wifi-hosted
// and sdkconfig.defaults.wifi-hosted.esp32s31).
//
// esp_wifi_remote turns every esp_wifi_remote_* call into an RPC to the
// co-processor, and the data path is an ordinary esp_netif. So DHCP, mDNS (its
// predefined WIFI_STA_DEF interface), the cloud client (it already checks
// WIFI_STA_DEF) and the web server need no Wi-Fi code of their own -- this
// file only joins the network and keeps it joined.
//
// Credentials live in the hub's NVS (namespace "wifi_sta"), not the
// co-processor's: swap the C5 and the hub still knows its network.
//
// Every call goes to the esp_wifi_remote_* twin and every event arrives on
// WIFI_REMOTE_EVENT. On the P4, which has no radio, the plain esp_wifi_*
// calls are one-line wrappers around those and WIFI_REMOTE_EVENT is
// WIFI_EVENT. On the S31 the plain calls would drive its own radio, which
// stays off (it shares the antenna with Zigbee).
#include "wifi_hosted.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#if CONFIG_ESP_WIFI_ENABLED
#include "esp_private/wifi.h"   // wifi_rxcb_t, for the S31 stand-in below
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ntp_cfg.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "zhac_task.h"

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

#if CONFIG_ESP_WIFI_ENABLED
// ── S31: Wi-Fi remote beside a native radio ─────────────────────────
// IDF 6.1 supports a remote Wi-Fi next to the native one (events on
// WIFI_REMOTE_EVENT, netif glue in esp_wifi/remote/esp_wifi_remote_net2.c),
// but ESP-Hosted (2.12.13, still in 3.0.9) assumes a host without Wi-Fi: it
// posts the co-processor's events on WIFI_EVENT and defines no
// WIFI_REMOTE_EVENT. Linking the native WIFI_EVENT, the native esp_wifi_stop
// that net2 registers as a shutdown handler, or the native
// esp_wifi_internal_reg_rxcb that esp_hosted's channel teardown calls, would
// pull in the whole native stack (libnet80211, libpp, ...), which the S31
// never starts and whose NOLOAD log strings break IDF v6.1's XIP-from-PSRAM
// link (esp-idf#18791, fixed on release/v6.1 after the v6.1 tag). So the four
// are defined here, and every event esp_hosted posts is passed on to
// WIFI_REMOTE_EVENT, where net2 and on_wifi listen. A second definition
// anywhere is a link error, not a silent mix-up.
ESP_EVENT_DEFINE_BASE(WIFI_EVENT);
ESP_EVENT_DEFINE_BASE(WIFI_REMOTE_EVENT);

extern "C" esp_err_t esp_wifi_stop(void) {
    return esp_wifi_remote_stop();
}

// Only reached when esp_hosted tears a channel down, to drop the native RX
// callback; the S31's RX path is net2's, which its disconnect handler drops.
extern "C" esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t, wifi_rxcb_t) {
    return ESP_OK;
}

// Only the events something here or in net2 reads; data sizes per the IDF type.
static void forward_event(void*, esp_event_base_t, int32_t id, void* data) {
    size_t n = 0;
    switch (id) {
    case WIFI_EVENT_STA_START:
    case WIFI_EVENT_STA_STOP:         break;
    case WIFI_EVENT_STA_CONNECTED:    n = sizeof(wifi_event_sta_connected_t); break;
    case WIFI_EVENT_STA_DISCONNECTED: n = sizeof(wifi_event_sta_disconnected_t); break;
    case WIFI_EVENT_SCAN_DONE:        n = sizeof(wifi_event_sta_scan_done_t); break;
    default: return;
    }
    esp_event_post(WIFI_REMOTE_EVENT, id, data, n, 0);
}

// The netif for the co-processor. Both are defined in net2 but declared in no
// header; the public esp_wifi_remote_create_default_sta() wraps them with
// if_key "WIFI_STA_RMT", which mDNS's predefined STA netif and remote_client's
// uplink check would not find. Our own config keeps "WIFI_STA_DEF".
extern "C" esp_netif_t* esp_netif_create_wifi_remote(wifi_interface_t wifi_if,
                                                     const esp_netif_inherent_config_t* cfg);
extern "C" esp_err_t esp_wifi_set_default_wifi_remote_sta_handlers(void);
#endif

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
    const esp_err_t e = esp_wifi_remote_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(e));
}

static void on_wifi(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) esp_wifi_remote_connect();
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
    esp_err_t e = esp_wifi_remote_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "co-processor did not answer (esp_wifi_init: %s) -- Wi-Fi off. "
                      "Check the C5 wiring and that it runs the matching ESP-Hosted image.",
                 esp_err_to_name(e));
        vTaskDelete(nullptr);
    }
    esp_wifi_remote_set_storage(WIFI_STORAGE_RAM);   // the host's NVS copy is the one that counts
    esp_wifi_remote_set_mode(WIFI_MODE_STA);
    if (s_ssid[0]) apply_creds();
    e = esp_wifi_remote_start();
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
        if (s_ssid[0] && !s_connected) esp_wifi_remote_connect();
    }
}

void wifi_hosted_start() {
    if (s_evt) return;   // idempotent
    s_evt = xEventGroupCreate();
    load_creds();

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.route_prio = kRoutePrio;
#if CONFIG_ESP_WIFI_ENABLED
    s_netif = esp_netif_create_wifi_remote(WIFI_IF_STA, &base);
#else
    s_netif = esp_netif_create_wifi(WIFI_IF_STA, &base);
#endif
    // Same DHCP hostname as the Ethernet side, so the router lists "zhac".
    esp_netif_set_hostname(s_netif, CONFIG_ZHAC_MDNS_HOSTNAME);
#if CONFIG_ESP_WIFI_ENABLED
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, forward_event, nullptr);
    esp_wifi_set_default_wifi_remote_sta_handlers();
#else
    esp_wifi_set_default_wifi_sta_handlers();
#endif

    esp_event_handler_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip, nullptr);

    // esp_wifi_init is an RPC that waits for the co-processor, so it runs
    // here and not in app_main. Stack in PSRAM where zhac_task allows it (the
    // S31): the task never touches flash. Its self-delete on a dead
    // co-processor leaves that stack behind, once per boot.
    if (zhac_task_create(wifi_task, "TaskWifi", 6144, nullptr, 5, nullptr) != pdPASS) {
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
    if (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
}

int wifi_hosted_scan(WifiAp* out, size_t max) {
    if (!s_ready || !out || max == 0) return -1;
    if (esp_wifi_remote_scan_start(nullptr, true) != ESP_OK) return -1;
    uint16_t n = static_cast<uint16_t>(max);
    auto* recs = static_cast<wifi_ap_record_t*>(std::calloc(n, sizeof(wifi_ap_record_t)));
    if (!recs) {
        esp_wifi_remote_clear_ap_list();
        return -1;
    }
    int got = 0;
    if (esp_wifi_remote_scan_get_ap_records(&n, recs) == ESP_OK) {
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
    esp_wifi_remote_disconnect();
    if (!s_ssid[0]) {
        ESP_LOGI(TAG, "network forgotten");
        return true;
    }
    apply_creds();
    esp_wifi_remote_connect();
    ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
    return true;
}
