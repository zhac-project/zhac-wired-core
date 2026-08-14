// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zhac-wired-core / main.cpp -- Tasks 1-2 boot.
//
// Goal: prove the P4 toolchain, PSRAM detection and partition table, then
// bring up the wired netif. HTTP/SPA, storage, rules, Lua and (in Phase 1)
// the Zigbee radio land in later tasks.
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "eth.h"

#include <cinttypes>

static const char* TAG = "zhac-wired";

static void log_chip_info() {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    ESP_LOGI(TAG, "chip: %s, rev v%u.%u, %d cores, flash %" PRIu32 " MB",
             CONFIG_IDF_TARGET,
             info.revision / 100,
             info.revision % 100,
             info.cores,
             flash_size / (1024 * 1024));
}

static void log_psram_info() {
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not initialized -- this build REQUIRES PSRAM");
        return;
    }
    ESP_LOGI(TAG, "PSRAM: initialized, %u MB",
             (unsigned)(esp_psram_get_size() / (1024 * 1024)));
}

static void log_heap_info() {
    ESP_LOGI(TAG, "heap internal: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "heap spiram:   free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "boot -- zhac-wired-core (Tasks 1-2 -- skeleton + Ethernet)");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase -- reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    log_chip_info();
    log_psram_info();
    log_heap_info();

    eth_start();

    // Report link/IP state periodically until the HTTP surface exists to
    // serve it (Task 5). Cheap, and makes the unplug/replug gate observable
    // from the serial monitor alone.
    while (true) {
        NetStatus net{};
        eth_get_status(&net);
        ESP_LOGI(TAG, "net: link=%d ip=%s speed=%" PRIu32 " duplex=%s mac=%s",
                 (int)net.link_up,
                 net.ip[0] ? net.ip : "-",
                 net.speed_mbps,
                 net.duplex_full ? "full" : "half",
                 net.mac[0] ? net.mac : "-");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
