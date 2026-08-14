// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "net_discovery.h"

#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "discovery";

static char s_hostname[64] = {0};

const char* net_discovery_hostname() { return s_hostname; }

void net_discovery_start(const char* hostname) {
    if (!hostname || !hostname[0]) hostname = "zhac";

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s -- the device will only be "
                      "reachable by IP address", esp_err_to_name(err));
        return;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set(%s) failed: %s",
                 hostname, esp_err_to_name(err));
        return;
    }
    snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);

    err = mdns_instance_name_set("ZHAC wired gateway");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
    }

    mdns_txt_item_t txt[] = {
        {"zhac", "1"},
        {"path", "/"},
    };
    err = mdns_service_add(nullptr, "_http", "_tcp", 80,
                           txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        // Name resolution still works without the service record; only
        // service-browse discovery (avahi-browse, Bonjour) is lost.
        ESP_LOGW(TAG, "mdns_service_add failed: %s -- name resolution only",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "advertising http://%s.local/", s_hostname);

    // Two units on one LAN both defaulting to "zhac" is a real collision.
    // mDNS conflict resolution will rename one of them, so the name printed
    // above is what to trust -- not the configured value. Set
    // CONFIG_ZHAC_MDNS_HOSTNAME per unit to avoid the ambiguity entirely.
}
