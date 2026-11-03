// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The NVS calls the stores under main/ make, over an in-memory map (nvs_fake.cpp). Every write is its
// own step, as on IDF (nvs_commit does nothing there), and fake_nvs_cut(n) fails every write after the
// next n: a power cut in the middle of an operation.
#pragma once
#include <cstddef>
#include <cstdint>

typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NVS_NOT_FOUND 0x1102

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_u32(nvs_handle_t h, const char* key, uint32_t* out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char* key, uint32_t v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* v, size_t len);
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key);

void fake_nvs_cut(int writes_left);   // < 0: no cut
void fake_nvs_clear();                // empty store, no cut
