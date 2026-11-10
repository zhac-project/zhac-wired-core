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
// co-processor's: swap the C5 and the hub still knows its network. The same
// copy sets the C5 up again whenever it restarts (power loss, crash, reset),
// and a C5 missing at boot is set up once it appears.
//
// Setup access point, for a hub with no cable: if neither Wi-Fi nor Ethernet
// has an address 30 s after Wi-Fi starts, an OPEN network "ZHAC-XXXX" comes up
// (owner's call, 2026-10-19: easy first connection beats a password printed on
// serial). A small DNS responder points every name at the hub, so phones show
// the web UI as a sign-in page (the SPA fallback answers their probe URLs);
// Settings > WiFi then scans and joins as usual. The AP closes once the hub has
// held a network for 2 min, or after 30 min in any case, and opens at most once
// per boot; once a network has held 2 min, it does not open again until the next
// boot, so an outage or a jammed Wi-Fi later on opens nothing.
//
// Only a fresh hub gets it after 30 s. A hub that is set up (a stored Wi-Fi
// network, or Ethernet held an address once) keeps retrying its network and
// opens the AP only after 15 min without any network, as a way back in when
// the network itself changed. So a power cut where the router boots slower
// than the hub opens nothing, and the AP never overlaps the 10-min password
// set-up window on a set-up hub. `ap_disabled` (sys_state) keeps it shut
// (review 2026-11-09 WN-01, WN-02).
//
// Every call goes to the esp_wifi_remote_* twin and every event arrives on
// WIFI_REMOTE_EVENT. On the P4, which has no radio, the plain esp_wifi_*
// calls are one-line wrappers around those and WIFI_REMOTE_EVENT is
// WIFI_EVENT. On the S31 the plain calls would drive its own radio, which
// stays off (it shares the antenna with Zigbee).
#include "wifi_hosted.h"

#include "esp_event.h"
#include "esp_hosted_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#if CONFIG_ESP_WIFI_ENABLED
#include "esp_private/wifi.h"   // wifi_rxcb_t, for the S31 stand-in below
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "ntp_cfg.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "zap_setup_window.h"
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
static constexpr EventBits_t kRetryBit   = BIT0;
static constexpr EventBits_t kRestartBit = BIT1;   // the co-processor booted

// ap-policy begin -- cut out by test/host/ap_policy/run.sh
static constexpr uint32_t kApAfterMs      = 30 * 1000;        // fresh hub: no network this long -> AP
static constexpr uint32_t kApAfterSetUpMs = 15 * 60 * 1000;   // set-up hub: only after this long
static constexpr uint32_t kApGraceMs      = 120 * 1000;       // network held this long -> AP off
static constexpr uint32_t kApMaxMs        = 30 * 60 * 1000;   // AP off after this, used or not

// +1 open the AP, -1 close it, 0 leave it. down_ms / up_ms: how long the hub
// has been without / with a network (0 while the other holds); open_ms: how
// long the AP has been up. settled: a network has held kApGraceMs this boot,
// after which a lost network (an outage, a jammed Wi-Fi) opens nothing until
// the next boot.
static int ap_step(bool ap_up, bool disabled, bool set_up, bool settled, uint32_t down_ms, uint32_t up_ms,
                   uint32_t open_ms) {
    if (!ap_up) return !disabled && !settled && down_ms >= (set_up ? kApAfterSetUpMs : kApAfterMs) ? 1 : 0;
    return disabled || up_ms >= kApGraceMs || open_ms >= kApMaxMs ? -1 : 0;
}
// ap-policy end
static_assert(kApAfterSetUpMs > kZapSetupWindowS * 1000u,
              "a set-up hub's AP must not open inside the password set-up window");

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
    case WIFI_EVENT_AP_START:
    case WIFI_EVENT_AP_STOP:          break;
    case WIFI_EVENT_AP_STACONNECTED:    n = sizeof(wifi_event_ap_staconnected_t); break;
    case WIFI_EVENT_AP_STADISCONNECTED: n = sizeof(wifi_event_ap_stadisconnected_t); break;
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
extern "C" esp_err_t esp_wifi_set_default_wifi_remote_ap_handlers(void);
#endif

static esp_netif_t*       s_netif    = nullptr;
static esp_netif_t*       s_ap_netif = nullptr;
static EventGroupHandle_t s_evt      = nullptr;
static volatile bool      s_ap       = false;   // setup access point up

// Written by the event handlers, the Wi-Fi task and wifi_hosted_connect();
// read anywhere without a lock -- same contract as eth_common's link flags.
static volatile bool s_ready     = false;
static volatile bool s_connected = false;
static volatile bool s_warned    = false;   // one log line per failure streak
static char s_ssid[33] = {};
static char s_pass[65] = {};
static bool s_eth_seen = false;             // Ethernet held an address once (NVS, never cleared)
static bool (*s_ap_disabled)() = nullptr;   // sys_ap_disabled, read at every decision

static void load_creds() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;   // never configured
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = '\0';
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = '\0';
    uint8_t eth = 0;
    s_eth_seen = nvs_get_u8(h, "eth_seen", &eth) == ESP_OK && eth;
    nvs_close(h);
}

// Once per hub: from then on it counts as set up, cable or not.
static void remember_eth() {
    s_eth_seen = true;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "eth_seen", 1) == ESP_OK) nvs_commit(h);
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
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "access point: a device joined");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "access point: a device left");
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

// ── Setup access point ──────────────────────────────────────────────
static bool eth_has_ip() {
    esp_netif_t* eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip{};
    return eth && esp_netif_is_netif_up(eth) &&
           esp_netif_get_ip_info(eth, &ip) == ESP_OK && ip.ip.addr != 0;
}

// Answers every DNS question with the AP's own address, so a phone's
// connectivity probe lands on the web UI. Runs while the AP is up.
static void dns_task(void*) {
    esp_netif_ip_info_t ip{};
    esp_netif_get_ip_info(s_ap_netif, &ip);
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(53);
    sa.sin_addr.s_addr = ip.ip.addr;   // AP side only, never the LAN
    timeval to{1, 0};                  // wake every second to see the AP close
    if (sock < 0 || setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to)) < 0 ||
        bind(sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "access point: DNS responder off (socket/bind failed)");
        if (sock >= 0) close(sock);
        vTaskDelete(nullptr);
    }
    uint8_t buf[512];
    while (s_ap) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 12 || n + 16 > static_cast<int>(sizeof(buf))) continue;   // timeout, junk or no room
        buf[2] = 0x81;               // response, authoritative
        buf[3] = 0x80;               // recursion available, no error
        buf[6] = 0; buf[7] = 1;      // one answer: A record for the first question
        static const uint8_t kAnswer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
        std::memcpy(buf + n, kAnswer, sizeof(kAnswer));
        std::memcpy(buf + n + sizeof(kAnswer), &ip.ip.addr, 4);
        sendto(sock, buf, n + 16, 0, reinterpret_cast<sockaddr*>(&from), flen);
    }
    close(sock);
    vTaskDelete(nullptr);
}

static void ap_config(wifi_config_t* ac) {
    uint8_t mac[6]{};
    esp_efuse_mac_get_default(mac);   // the hub's MAC, so the name matches its Ethernet MAC
    ac->ap.ssid_len = static_cast<uint8_t>(std::snprintf(reinterpret_cast<char*>(ac->ap.ssid),
                                                         sizeof(ac->ap.ssid), "ZHAC-%02X%02X", mac[4], mac[5]));
    ac->ap.channel        = 1;        // follows the station's channel once it joins
    ac->ap.authmode       = WIFI_AUTH_OPEN;
    ac->ap.max_connection = 4;
}

static void ap_open() {
    if (!s_ap_netif) {
        esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();   // 192.168.4.1, DHCP server
#if CONFIG_ESP_WIFI_ENABLED
        s_ap_netif = esp_netif_create_wifi_remote(WIFI_IF_AP, &base);
        esp_wifi_set_default_wifi_remote_ap_handlers();
#else
        s_ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &base);
        esp_wifi_set_default_wifi_ap_handlers();
#endif
    }
    wifi_config_t ac{};
    ap_config(&ac);
    // Stop, reconfigure, start: switching the mode on a running radio would
    // briefly put up an AP with the co-processor's default name.
    esp_wifi_remote_stop();
    esp_err_t e = esp_wifi_remote_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_remote_set_config(WIFI_IF_AP, &ac);
    if (e == ESP_OK) e = esp_wifi_remote_start();   // STA_START -> on_wifi rejoins a stored network
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "access point failed: %s", esp_err_to_name(e));
        esp_wifi_remote_set_mode(WIFI_MODE_STA);
        esp_wifi_remote_start();
        return;
    }
    s_ap = true;
    ESP_LOGW(TAG, "no network -- open access point \"%s\" up: join it and the web UI "
                  "opens (or browse to 192.168.4.1), then Settings > WiFi", ac.ap.ssid);
    // Same stack rule as TaskWifi; the self-delete when the AP closes leaves
    // that stack behind on the S31, once per boot at most.
    if (zhac_task_create(dns_task, "TaskApDns", 3072, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "access point: DNS responder off (task create failed)");
    }
}

static void ap_close(const char* why) {
    s_ap = false;   // dns_task sees it within a second
    esp_wifi_remote_set_mode(WIFI_MODE_STA);   // the station stays joined
    ESP_LOGI(TAG, "access point closed (%s) -- not again until the next boot", why);
}

// ── Co-processor (re)start ──────────────────────────────────────────
// Everything the co-processor forgets when it restarts: the hub's copy of the
// network, the mode (with the setup AP when it is up) and the power setting.
static bool bring_up() {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_remote_init(&cfg);
    if (e == ESP_OK) {
        esp_wifi_remote_set_storage(WIFI_STORAGE_RAM);   // the host's NVS copy is the one that counts
        e = esp_wifi_remote_set_mode(s_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    }
    if (e == ESP_OK && s_ap) {
        wifi_config_t ac{};
        ap_config(&ac);
        e = esp_wifi_remote_set_config(WIFI_IF_AP, &ac);
    }
    if (e == ESP_OK) {
        if (s_ssid[0]) apply_creds();
        e = esp_wifi_remote_start();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "co-processor did not answer (%s) -- Wi-Fi off until it starts. "
                      "Check the C5 wiring and that it runs the matching ESP-Hosted image.",
                 esp_err_to_name(e));
        return false;
    }
    // Mains-powered hub: no modem sleep. With the default it answers ~100 ms
    // late (it wakes only for DTIM beacons), which the web UI and WS feel.
    esp_wifi_remote_set_ps(WIFI_PS_NONE);
    s_ready = true;
    if (s_ssid[0]) ESP_LOGI(TAG, "started -- joining \"%s\"", s_ssid);
    else           ESP_LOGI(TAG, "started -- no network yet (web UI: Settings > WiFi)");
    return true;
}

// esp_hosted posts this whenever the co-processor boots: once while the first
// bring_up() waits for it (the hub resets the C5 at start), and again after the
// C5 crashed, lost power or was reset. Only the later ones are restarts.
static volatile bool s_first_try_done = false;
static void on_cp_init(void*, esp_event_base_t, int32_t, void*) {
    if (s_first_try_done) xEventGroupSetBits(s_evt, kRestartBit);
}

// A restarted co-processor sends no stop events for the interfaces it lost, so
// they are posted here: the netif glue then stops the interfaces and the next
// start brings them up clean, as after an ordinary stop.
static void restart() {
    ESP_LOGW(TAG, "co-processor restarted -- setting Wi-Fi up again");
    if (s_ready) {
        s_ready = false;
        if (s_connected) {
            wifi_event_sta_disconnected_t d{};
            d.reason = WIFI_REASON_UNSPECIFIED;
            esp_event_post(WIFI_REMOTE_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d, sizeof(d), portMAX_DELAY);
        }
        esp_event_post(WIFI_REMOTE_EVENT, WIFI_EVENT_STA_STOP, nullptr, 0, portMAX_DELAY);
        if (s_ap) esp_event_post(WIFI_REMOTE_EVENT, WIFI_EVENT_AP_STOP, nullptr, 0, portMAX_DELAY);
    }
    s_connected = false;
    bring_up();
}

static void wifi_task(void*) {
    bring_up();   // a failure here (no C5) waits for its boot-up announcement
    s_first_try_done = true;

    // Once a second: redo the set-up after a co-processor restart, retry the
    // station when asked, and decide the setup AP (ap_step above), counting
    // from the moment Wi-Fi first started. Since-stamps use 0 for "not now".
    bool started = s_ready;
    TickType_t down_since = 0, up_since = 0, opened = 0;
    bool ap_done = false;   // the AP opens at most once per boot
    bool settled = false;   // a network held kApGraceMs this boot
    for (;;) {
        const EventBits_t bits = xEventGroupWaitBits(s_evt, kRetryBit | kRestartBit, pdTRUE, pdFALSE,
                                                     pdMS_TO_TICKS(1000));
        if (bits & kRestartBit) {
            restart();
        } else if (bits & kRetryBit) {
            vTaskDelay(pdMS_TO_TICKS(kRetryMs));
            if (s_ready && s_ssid[0] && !s_connected) esp_wifi_remote_connect();
        }
        const bool eth = eth_has_ip();
        if (eth && !s_eth_seen) remember_eth();
        started = started || s_ready;
        if (!started || ap_done) continue;
        const TickType_t now = xTaskGetTickCount();
        if (s_connected || eth) { down_since = 0; if (!up_since) up_since = now | 1; }
        else                    { up_since = 0; if (!down_since) down_since = now | 1; }
        const auto held = [now](TickType_t since) {
            return since ? static_cast<uint32_t>(pdTICKS_TO_MS(now - since)) : 0u;
        };
        settled = settled || held(up_since) >= kApGraceMs;
        const bool disabled = s_ap_disabled && s_ap_disabled();
        const int step = ap_step(s_ap, disabled, s_ssid[0] || s_eth_seen, settled, held(down_since),
                                 held(up_since), held(opened));
        if (step > 0) {
            ap_open();
            opened = now | 1;
            if (!s_ap) ap_done = true;   // the AP failed: no second try
        } else if (step < 0) {
            ap_close(disabled                      ? "turned off in settings"
                     : held(up_since) >= kApGraceMs ? "network held"
                                                    : "open too long");
            ap_done = true;
        }
    }
}

void wifi_hosted_start(bool (*ap_disabled)()) {
    if (s_evt) return;   // idempotent
    s_ap_disabled = ap_disabled;
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

    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_CP_INIT, on_cp_init, nullptr);
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
