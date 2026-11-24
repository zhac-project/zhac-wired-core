// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub: the "alarm" namespace as a map; every write goes into the trace; writes can be made to fail.
#pragma once
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "esp_err.h"
#include "harness.h"
typedef int nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
extern std::map<std::string, std::string> g_nvs;      // key -> blob (namespace "alarm" only)
extern bool                               g_nvs_fail;       // every write fails
extern std::string                        g_nvs_fail_key;   // writes of this key fail
inline esp_err_t nvs_open(const char* ns, int, nvs_handle_t* h) { OUTSIDE_LOCK("nvs_open"); *h = 1; return std::strcmp(ns, "alarm") == 0 ? ESP_OK : ESP_FAIL; }
inline esp_err_t nvs_set_blob(nvs_handle_t, const char* k, const void* d, size_t n) {
    if (g_nvs_fail || g_nvs_fail_key == k) return ESP_FAIL;
    g_nvs[k] = std::string(static_cast<const char*>(d), n);
    g_trace.push_back(std::string("nvs:") + k);
    return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char* k, void* d, size_t* n) {
    auto it = g_nvs.find(k);
    if (it == g_nvs.end() || it->second.size() > *n) return ESP_FAIL;
    std::memcpy(d, it->second.data(), it->second.size());
    *n = it->second.size();
    return ESP_OK;
}
