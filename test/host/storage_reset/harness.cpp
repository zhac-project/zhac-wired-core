// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// erase_storage_keeping_pin() against a fake of the NVS calls it makes: two partitions, a key-value map for
// "nvs", an init flag per partition, and the order of the erases.
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using esp_err_t    = int;
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_INITIALIZED = 0x1101, ESP_ERR_NVS_NOT_FOUND = 0x1102;
struct esp_partition_t { int dummy; };
enum { ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_NVS = 2 };
#define ESP_LOGE(tag, ...) (std::printf("E %s: ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
static const char* TAG = "ws_bridge";

static std::map<std::string, uint64_t> g_nvs;   // "ns/key" -> value
static bool g_nvs_init = true, g_store_erased = false, g_store_erase_fails = false;
static std::vector<std::string> g_order;
static std::string g_open_ns;

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* h) {
    if (!g_nvs_init) return ESP_ERR_NVS_NOT_INITIALIZED;
    if (mode == NVS_READONLY) {
        bool any = false;
        for (auto& kv : g_nvs) any |= kv.first.rfind(std::string(ns) + "/", 0) == 0;
        if (!any) return ESP_ERR_NVS_NOT_FOUND;
    }
    g_open_ns = ns;
    *h = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return g_nvs_init ? ESP_OK : ESP_ERR_NVS_NOT_INITIALIZED; }
esp_err_t nvs_get_u64(nvs_handle_t, const char* key, uint64_t* v) {
    auto it = g_nvs.find(g_open_ns + "/" + key);
    if (it == g_nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
    *v = it->second;
    return ESP_OK;
}
esp_err_t nvs_set_u64(nvs_handle_t, const char* key, uint64_t v) {
    if (!g_nvs_init) return ESP_ERR_NVS_NOT_INITIALIZED;
    g_nvs[g_open_ns + "/" + key] = v;
    return ESP_OK;
}
static esp_partition_t g_store_part{};
const esp_partition_t* esp_partition_find_first(int, int, const char* label) {
    return std::string(label) == "zhac_data" ? &g_store_part : nullptr;
}
esp_err_t nvs_flash_erase_partition(const char* label) {
    g_order.push_back(label);
    if (g_store_erase_fails) return ESP_FAIL;
    g_store_erased = true;
    return ESP_OK;
}
esp_err_t nvs_flash_erase() { g_order.push_back("nvs"); g_nvs.clear(); g_nvs_init = false; return ESP_OK; }
esp_err_t nvs_flash_init() { g_nvs_init = true; return ESP_OK; }
esp_err_t nvs_flash_deinit() { g_nvs_init = false; return ESP_OK; }
// esp_zigbee_backend's pin reader: NVS while mounted, else what the partition bytes hold (P1 review 2026-11-19 #2).
static uint64_t g_raw_pin = 0;
uint64_t esp_zigbee_backend_coordinator_pin(void) {
    if (!g_nvs_init) return g_raw_pin;
    const auto it = g_nvs.find("zb_coord/ieee");
    return it == g_nvs.end() ? 0 : it->second;
}

#include "slice.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static void reset() {
    g_nvs.clear(); g_order.clear();
    g_nvs_init = true; g_store_erased = false; g_store_erase_fails = false; g_raw_pin = 0;
}
// What the restart's flush handlers would manage to write after the erase.
static bool late_write_lands() {
    nvs_handle_t h;
    return nvs_open("rules", NVS_READWRITE, &h) == ESP_OK && nvs_set_u64(h, "r1", 1) == ESP_OK;
}

int main() {
    reset();
    g_nvs["zb_coord/ieee"] = 0x00124B0001020304ull;
    g_nvs["rules/r1"] = 7;
    g_nvs["sys_cfg/auth"] = 9;
    expect("erase succeeds", erase_storage_keeping_pin() == ESP_OK);
    expect("the device store is erased, before nvs",
           g_store_erased && g_order.size() == 2 && g_order[0] == "zhac_data" && g_order[1] == "nvs");
    expect("the coordinator pin survives the erase",
           g_nvs.count("zb_coord/ieee") && g_nvs["zb_coord/ieee"] == 0x00124B0001020304ull);
    expect("nothing else survives", g_nvs.size() == 1);
    expect("NVS is left shut: the restart's flush writes nothing back", !late_write_lands());

    reset();
    g_nvs["rules/r1"] = 7;
    expect("no pin: erase succeeds", erase_storage_keeping_pin() == ESP_OK);
    expect("  and nvs is empty", g_nvs.empty());
    expect("  and shut", !late_write_lands());

    reset();
    g_nvs_init = false;   // the boot found NVS unusable (storage_error): nothing can be read first
    expect("unreadable NVS: erase still succeeds", erase_storage_keeping_pin() == ESP_OK && g_store_erased);
    expect("  and leaves it shut", !late_write_lands());

    reset();
    g_nvs_init = false;                   // NVS full at boot (NO_FREE_PAGES): not mounted, Settings offers the erase
    g_raw_pin  = 0x00124B0001020304ull;   // the pin is still in its bytes
    expect("NVS not mounted: erase succeeds", erase_storage_keeping_pin() == ESP_OK);
    expect("  and keeps the pin read from the partition",
           g_nvs.count("zb_coord/ieee") && g_nvs["zb_coord/ieee"] == 0x00124B0001020304ull);
    expect("  and leaves it shut", !late_write_lands());

    reset();
    g_nvs["zb_coord/ieee"] = 0x1122ull;
    g_store_erase_fails = true;
    expect("the store erase fails: the error comes back", erase_storage_keeping_pin() != ESP_OK);
    expect("  and nvs is not touched", g_order.size() == 1 && g_nvs.count("zb_coord/ieee"));
    return g_fail;
}
