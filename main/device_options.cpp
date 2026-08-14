// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "device_options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ArduinoJson.h"
#include "device_shadow.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "device_options";
static const char* kNamespace = "zhac_opt";

static nvs_handle_t s_nvs   = 0;
static bool         s_open  = false;

static nvs_handle_t opts_nvs() {
    if (!s_open) {
        if (nvs_open(kNamespace, NVS_READWRITE, &s_nvs) == ESP_OK) {
            s_open = true;
        } else {
            ESP_LOGW(TAG, "nvs_open(%s) failed — options not persisted", kNamespace);
        }
    }
    return s_open ? s_nvs : 0;
}

// Matches net-core's key scheme for store compatibility.
static void key_for(uint64_t ieee, char out[16]) {
    snprintf(out, 16, "%014llx", (unsigned long long)ieee);
}

// Apply the present option fields to device_shadow. `flood_protection` is
// expressed as a debounce window (per device_shadow's own contract); an
// explicit `debounce_ms` takes precedence if both are present.
static bool apply_opts(uint64_t ieee, const JsonDocument& doc) {
    bool ok = true;
    if (doc["occupancy_timeout"].is<int>()) {
        ok = device_shadow_set_occupancy_timeout(
                 ieee, (uint16_t)doc["occupancy_timeout"].as<int>()) && ok;
    }
    if (doc["flood_protection"].is<int>()) {
        ok = device_shadow_set_debounce_ms(
                 ieee, (uint32_t)doc["flood_protection"].as<int>()) && ok;
    }
    if (doc["debounce_ms"].is<int>()) {
        ok = device_shadow_set_debounce_ms(
                 ieee, (uint32_t)doc["debounce_ms"].as<int>()) && ok;
    }
    if (doc["throttle_ms"].is<int>()) {
        ok = device_shadow_set_throttle_ms(
                 ieee, (uint32_t)doc["throttle_ms"].as<int>()) && ok;
    }
    return ok;
}

bool device_options_set(uint64_t ieee, const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) {
        ESP_LOGW(TAG, "bad options json");
        return false;
    }
    const bool applied = apply_opts(ieee, doc);

    nvs_handle_t h = opts_nvs();
    if (h) {
        char key[16];
        key_for(ieee, key);
        char blob[192];
        const size_t n = serializeJson(doc, blob, sizeof(blob));
        if (n > 0 && n < sizeof(blob)) {
            if (nvs_set_str(h, key, blob) != ESP_OK || nvs_commit(h) != ESP_OK) {
                ESP_LOGW(TAG, "persist failed for %s", key);
            }
        }
    }
    return applied;
}

size_t device_options_get_json(uint64_t ieee, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    nvs_handle_t h = opts_nvs();
    if (!h) return 0;
    char key[16];
    key_for(ieee, key);
    size_t sz = cap;
    if (nvs_get_str(h, key, out, &sz) != ESP_OK) {
        out[0] = '\0';
        return 0;
    }
    return sz > 0 ? sz - 1 : 0;   // nvs_get_str size includes the NUL
}

void device_options_restore_all() {
    nvs_handle_t h = opts_nvs();
    if (!h) return;

    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace,
                                 NVS_TYPE_STR, &it);
    uint16_t restored = 0;
    while (e == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        const uint64_t ieee = strtoull(info.key, nullptr, 16);
        if (ieee) {
            char blob[192];
            size_t sz = sizeof(blob);
            if (nvs_get_str(h, info.key, blob, &sz) == ESP_OK) {
                JsonDocument doc;
                if (!deserializeJson(doc, blob, sz)) {
                    apply_opts(ieee, doc);
                    restored++;
                }
            }
        }
        e = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    ESP_LOGI(TAG, "restored options for %u device(s)", (unsigned)restored);
}
