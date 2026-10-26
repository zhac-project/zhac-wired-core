// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The fake NVS of include/nvs.h: one map for every namespace (each store under test has its own keys).
#include "nvs.h"
#include <cstring>
#include <map>
#include <string>
#include <vector>

static std::map<std::string, std::vector<uint8_t>> g_kv;
static int g_writes_left = -1;
void fake_nvs_cut(int writes_left) { g_writes_left = writes_left; }
void fake_nvs_clear() { g_kv.clear(); g_writes_left = -1; }
static bool write_ok() {
    if (g_writes_left < 0) return true;
    if (g_writes_left == 0) return false;
    g_writes_left--;
    return true;
}
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* out) { *out = 1; return ESP_OK; }
void      nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t, const char* key, uint32_t* out) {
    auto it = g_kv.find(key);
    if (it == g_kv.end() || it->second.size() != 4) return ESP_ERR_NVS_NOT_FOUND;
    std::memcpy(out, it->second.data(), 4);
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t, const char* key, uint32_t v) {
    if (!write_ok()) return ESP_FAIL;
    g_kv[key].assign(reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4);
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* out, size_t* len) {
    auto it = g_kv.find(key);
    if (it == g_kv.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < it->second.size()) return ESP_FAIL;
    std::memcpy(out, it->second.data(), it->second.size());
    *len = it->second.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* v, size_t len) {
    if (!write_ok()) return ESP_FAIL;
    g_kv[key].assign(static_cast<const uint8_t*>(v), static_cast<const uint8_t*>(v) + len);
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char* key) {
    if (!write_ok()) return ESP_FAIL;
    return g_kv.erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
