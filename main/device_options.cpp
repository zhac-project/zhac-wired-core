// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "device_options.h"
#include "device_options_key.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ArduinoJson.h"
#include "device_shadow.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "zigbee_pool.h"

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

// The whole 64-bit address (device_options_key.h, review F19).
static void key_for(uint64_t ieee, char out[16]) { opt_key(ieee, out); }

// Option ranges. Out of range is refused, not wrapped: a negative throttle
// used to become 4294967295 ms -- updates suppressed for weeks (review F26).
static constexpr int kMaxSeconds = 65535;
static constexpr int kMaxMs      = 3600 * 1000;   // an hour

static bool in_range(const JsonDocument& doc, const char* key, int max) {
    if (doc[key].isNull()) return true;                     // not part of this call
    if (!doc[key].is<int>()) return false;
    const int v = doc[key].as<int>();
    return v >= 0 && v <= max;
}

// Every option present must be valid before any is applied: a request with
// one bad field must not half-change the device.
static bool opts_valid(const JsonDocument& doc) {
    return in_range(doc, "occupancy_timeout", kMaxSeconds) &&
           in_range(doc, "no_occupancy_since", kMaxSeconds) &&
           in_range(doc, "flood_protection", kMaxMs) &&
           in_range(doc, "debounce_ms", kMaxMs) &&
           in_range(doc, "throttle_ms", kMaxMs);
}

// Apply the present option fields to device_shadow. `flood_protection` is
// expressed as a debounce window (per device_shadow's own contract); an
// explicit `debounce_ms` takes precedence if both are present.
static bool apply_opts(uint64_t ieee, const JsonDocument& doc) {
    bool ok = true;
    if (doc["occupancy_timeout"].is<int>()) {
        // Seconds, 0 = never clear occupancy. Out of range is refused rather
        // than wrapped into some other interval.
        const int s = doc["occupancy_timeout"].as<int>();
        ok = s >= 0 && s <= 65535 &&
             device_shadow_set_occupancy_timeout(ieee, static_cast<uint16_t>(s)) && ok;
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
    if (doc["no_occupancy_since"].is<int>()) {
        // Seconds of quiet after "no motion", 0 = off. Lives only here: the
        // shadow keeps it in RAM, so this store is what survives a reboot.
        const int s = doc["no_occupancy_since"].as<int>();
        ok = s >= 0 && s <= 65535 &&
             device_shadow_set_no_occupancy_since(ieee, static_cast<uint16_t>(s)) && ok;
    }
    return ok;
}

bool device_options_set(uint64_t ieee, const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len) || !doc.is<JsonObject>()) {
        ESP_LOGW(TAG, "bad options json");
        return false;
    }
    if (!opts_valid(doc)) {
        ESP_LOGW(TAG, "options out of range -- nothing changed");
        return false;
    }
    const bool applied = apply_opts(ieee, doc);

    // A save that did not reach flash is a failure: the option would be gone
    // after the next reboot.
    bool persisted = false;
    nvs_handle_t h = opts_nvs();
    if (h) {
        char key[16];
        key_for(ieee, key);
        char blob[192];
        // Merge into what is stored: the web UI sends one option per call,
        // and the stored object is what device_options_restore_all() applies
        // at boot -- replacing it would forget every other option.
        JsonDocument stored;
        size_t sz = sizeof(blob);
        if (nvs_get_str(h, key, blob, &sz) != ESP_OK ||
            deserializeJson(stored, blob, sz) || !stored.is<JsonObject>()) {
            stored.to<JsonObject>();
        }
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) stored[kv.key()] = kv.value();
        stored.remove("ieee");   // the NVS key names the device
        const size_t n = serializeJson(stored, blob, sizeof(blob));
        if (n > 0 && n < sizeof(blob)) {
            persisted = nvs_set_str(h, key, blob) == ESP_OK && nvs_commit(h) == ESP_OK;
        }
        if (!persisted) ESP_LOGW(TAG, "persist failed for %s", key);
    }
    return applied && persisted;
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

// Move records saved under the old hex keys (review F19) to the new ones,
// once. The owner is found among the devices in the pool; a key two devices
// shared cannot be told apart and is dropped, as is one of a device that is
// gone (restoring it made up a phantom device).
static void migrate_old_keys(nvs_handle_t h) {
    constexpr int kMaxOld = 32;
    char old_keys[kMaxOld][16];
    int n_old = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace, NVS_TYPE_STR, &it);
    while (e == ESP_OK && it && n_old < kMaxOld) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint64_t unused;
        if (!opt_key_parse(info.key, &unused)) {
            std::snprintf(old_keys[n_old++], sizeof(old_keys[0]), "%.15s", info.key);
        }
        e = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    if (!n_old) return;

    static uint64_t known[256];   // static: one boot-time pass, keep it off the stack
    int n_known = 0;
    zigbee_pool_lock();
    const ZapDevice* all = pool_all();
    for (uint16_t i = 0; i < pool_count() && n_known < 256; i++) known[n_known++] = all[i].ieee_addr;
    zigbee_pool_unlock();

    for (int i = 0; i < n_old; i++) {
        uint64_t owner = 0;
        const int found = opt_old_key_owner(old_keys[i], known, n_known, &owner);
        if (found < 0) continue;   // not an old options key: leave it alone
        char blob[192];
        size_t sz = sizeof(blob);
        if (found == 1 && nvs_get_str(h, old_keys[i], blob, &sz) == ESP_OK) {
            char key[16];
            key_for(owner, key);
            size_t exists = 0;
            const bool kept = nvs_get_str(h, key, nullptr, &exists) == ESP_OK ||   // newer record wins
                              nvs_set_str(h, key, blob) == ESP_OK;
            if (!kept) {
                ESP_LOGW(TAG, "options of %016llx could not be moved -- old key kept", (unsigned long long)owner);
                continue;
            }
            ESP_LOGI(TAG, "options of %016llx moved to key %s", (unsigned long long)owner, key);
        } else {
            ESP_LOGW(TAG, "old options key %s: %s -- dropped", old_keys[i],
                     found == 0 ? "no such device" : found > 1 ? "shared by two devices" : "unreadable");
        }
        nvs_erase_key(h, old_keys[i]);
    }
    nvs_commit(h);
}

void device_options_restore_all() {
    nvs_handle_t h = opts_nvs();
    if (!h) return;
    migrate_old_keys(h);

    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace,
                                 NVS_TYPE_STR, &it);
    uint16_t restored = 0;
    while (e == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint64_t ieee = 0;
        if (opt_key_parse(info.key, &ieee) && ieee) {
            char blob[192];
            size_t sz = sizeof(blob);
            if (nvs_get_str(h, info.key, blob, &sz) == ESP_OK) {
                JsonDocument doc;
                if (!deserializeJson(doc, blob, sz) && opts_valid(doc)) {
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
