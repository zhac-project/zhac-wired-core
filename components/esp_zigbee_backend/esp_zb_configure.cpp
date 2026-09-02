// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_configure -- the configure half of P2. Bindings, reporting and
// declarative config steps for the esp-zigbee backend.
//
// WHY THIS EXISTS
// ---------------
// The interview tells us WHAT a device is. Configure is what makes it
// actually talk: without a binding the device has no reason to send its
// reports to us, and without Configure Reporting a mains-powered device
// reports on its own (usually never) cadence. A thermostat or metering plug
// that is interviewed but not configured looks paired and does nothing.
//
// zhc_adapter owns the policy: `zhac_adapter_configure()` resolves the
// PreparedDefinition and walks its `bindings[]`, `reports[]` and
// `config_steps[]`, firing one registered transport per entry. This file is
// only those transports. That is the same split the ZNP path uses
// (zhc_configure_bridge.cpp), so every one of the 6,235 definitions gets
// identical treatment on both radios -- which is the entire point of §6.6's
// parity requirement.
//
// The difference from the ZNP bridge is that it delegates to zcl_commands.cpp,
// which builds ZNP AF_DATA_REQUEST frames. Here the ZCL frames are built
// directly and handed to esp_zb_af_send(), because APS is the only transport
// this backend has.
//
// BLOCKING IS INTENTIONAL
// -----------------------
// run_configure expects a synchronous bool per entry, and the ZDO bind API is
// asynchronous. Configure only ever runs from the interview task (it is
// invoked at the end of an interview), so blocking that one task on a
// semaphore is safe and keeps the step walker's contract intact. One static
// wait slot is sufficient for the same reason the interview has one.
#include "sdkconfig.h"

#if CONFIG_ZHAC_ESP_ZIGBEE

#include "esp_zb_interview.h"   // esp_zb_af_send
#include "esp_zb_lock.h"
#include "esp_zb_zcl_frame.h"

#include "esp_log.h"
#include "ezbee/zdo/zdo_bind_mgmt.h"
#include "ezbee/zdo/zdo_type.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "zap_common.h"
#include "zcl_seq.h"
#include "zhc_adapter.h"
#include "zigbee_mgr.h"      // zigbee_mgr_coordinator_ieee
#include "zigbee_pool.h"

#include <cstring>

static const char* TAG = "esp_zb_cfg";

namespace {

// Largest ZCL body we build here. Config-step payloads are small (Tuya magic
// packets, IAS CIE writes); 128 covers every def in the library with room.
constexpr size_t kZclMax = 128;

constexpr uint32_t kBindTimeoutMs = 5000;

SemaphoreHandle_t s_bind_sem = nullptr;
bool              s_bind_ok  = false;

// Resolve a device's current short address. Snapshot rather than holding a
// pool pointer -- pool_remove()'s swap-with-last can retarget the slot.
bool nwk_of(uint64_t ieee, uint16_t* out) {
    ZapDevice snap{};
    if (!zigbee_pool_snapshot(ieee, &snap)) return false;
    *out = snap.nwk_addr;
    return snap.nwk_addr != 0;
}

void on_bind_result(const ezb_zdp_bind_req_result_t* r, void*) {
    s_bind_ok = r && r->error == 0 && r->rsp &&
                r->rsp->status == EZB_ZDP_STATUS_SUCCESS;
    xSemaphoreGive(s_bind_sem);
}

// Shared by bind and unbind -- the request structs are the same type.
bool do_bind(uint16_t nwk, uint64_t src_ieee, uint8_t src_ep, uint16_t cluster,
             uint64_t dst_ieee, uint8_t dst_ep, bool unbind) {
    if (!s_bind_sem) return false;

    ezb_zdo_bind_req_t req{};
    req.dst_nwk_addr           = nwk;
    req.field.src_addr.u64     = src_ieee;
    req.field.src_ep           = src_ep;
    req.field.cluster_id       = cluster;
    req.field.dst_addr_mode    = 0x03;   // 64-bit address + endpoint present
    req.field.dst_addr.extended_addr.u64 = dst_ieee;
    req.field.dst_ep           = dst_ep;
    req.cb                     = on_bind_result;

    xSemaphoreTake(s_bind_sem, 0);       // drain a stale post
    s_bind_ok = false;
    int e = -1;
    {
        // Lock only around the request: the bind result arrives via a
        // callback on the stack task, which needs the lock to run.
        zb_lock::Guard g;
        if (g) e = static_cast<int>(unbind ? ezb_zdo_unbind_req(&req)
                                           : ezb_zdo_bind_req(&req));
    }
    if (e != 0) {
        ESP_LOGW(TAG, "%s req failed to send (%d)", unbind ? "unbind" : "bind",
                 (int)e);
        return false;
    }
    if (xSemaphoreTake(s_bind_sem, pdMS_TO_TICKS(kBindTimeoutMs)) != pdTRUE) {
        ESP_LOGW(TAG, "%s 0x%04x ep %u cluster 0x%04x timed out",
                 unbind ? "unbind" : "bind", nwk, src_ep, cluster);
        return false;
    }
    return s_bind_ok;
}

// ── Transports registered with zhc_adapter ────────────────────────────────

bool cfg_bind(uint64_t ieee, uint8_t ep, uint16_t cluster) {
    uint16_t nwk = 0;
    if (!nwk_of(ieee, &nwk)) {
        ESP_LOGW(TAG, "bind: unknown ieee %016llx", (unsigned long long)ieee);
        return false;
    }
    const uint64_t coord = zigbee_mgr_coordinator_ieee();
    if (coord == 0) {
        ESP_LOGW(TAG, "bind: coordinator ieee not known yet");
        return false;
    }
    // Destination is always the coordinator's endpoint 1 -- that is the
    // endpoint this firmware registers and the one reports must land on.
    const bool ok = do_bind(nwk, ieee, ep, cluster, coord, 1, /*unbind=*/false);
    ESP_LOGI(TAG, "bind %016llx ep %u cluster 0x%04x -> coordinator: %s",
             (unsigned long long)ieee, ep, cluster, ok ? "ok" : "FAILED");
    return ok;
}

bool cfg_report(uint64_t ieee, uint8_t ep, uint16_t cluster, uint16_t attr_id,
                uint8_t attr_type, uint16_t min_interval, uint16_t max_interval,
                uint32_t reportable_change, uint16_t manu) {
    uint16_t nwk = 0;
    if (!nwk_of(ieee, &nwk)) return false;

    uint8_t f[kZclMax];
    const size_t p = zhc_zcl::configure_report(
        f, sizeof(f), zcl_seq_next(), attr_id, attr_type,
        min_interval, max_interval, reportable_change, manu);
    if (p == 0) {
        ESP_LOGW(TAG, "report: unsupported attr type 0x%02x on cluster 0x%04x "
                      "attr 0x%04x", attr_type, cluster, attr_id);
        return false;
    }

    // Fire and forget, like z2m's reporting.* helpers. The device's Configure
    // Reporting Response carries per-attribute status and arrives later as a
    // normal inbound frame through the decode pipeline.
    const bool ok = esp_zb_af_send(nwk, ep, cluster, f, p);
    ESP_LOGI(TAG, "report %016llx ep %u cluster 0x%04x attr 0x%04x "
                  "min=%us max=%us chg=%lu: %s",
             (unsigned long long)ieee, ep, cluster, attr_id,
             min_interval, max_interval,
             (unsigned long)reportable_change, ok ? "sent" : "FAILED");
    return ok;
}

bool cfg_cmd(uint64_t /*ieee*/, uint16_t nwk, uint8_t ep, uint16_t cluster,
             uint8_t cmd_id, const uint8_t* payload, uint8_t payload_len,
             uint8_t flags) {
    // No manufacturer code: the ZNP cmd transport does not carry one either,
    // so definitions never supply it here.
    uint8_t f[kZclMax];
    const size_t p = zhc_zcl::cluster_command(f, sizeof(f), zcl_seq_next(),
                                              cmd_id, payload, payload_len, flags);
    if (p == 0) return false;
    return esp_zb_af_send(nwk, ep, cluster, f, p);
}

bool cfg_read(uint64_t /*ieee*/, uint16_t nwk, uint8_t ep, uint16_t cluster,
              const uint8_t* attr_ids_le, uint8_t attr_count, uint16_t manu) {
    uint8_t f[kZclMax];
    const size_t p = zhc_zcl::read_attributes(f, sizeof(f), zcl_seq_next(),
                                              attr_ids_le, attr_count, manu);
    if (p == 0) return false;
    return esp_zb_af_send(nwk, ep, cluster, f, p);
}

bool cfg_write(uint64_t /*ieee*/, uint16_t nwk, uint8_t ep, uint16_t cluster,
               uint16_t attr_id, uint8_t attr_type, const uint8_t* val,
               uint8_t len, uint16_t manu) {
    // manu != 0 builds a manufacturer-specific frame. Required for lumi 0xFCC0
    // writes, which Aqara hardware silently rejects when sent profile-wide.
    uint8_t f[kZclMax];
    const size_t p = zhc_zcl::write_attribute(f, sizeof(f), zcl_seq_next(),
                                              attr_id, attr_type, val, len, manu);
    if (p == 0) return false;
    return esp_zb_af_send(nwk, ep, cluster, f, p);
}

void cfg_sleep(uint16_t wait_ms) {
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

}  // namespace

void esp_zb_configure_register() {
    if (!s_bind_sem) s_bind_sem = xSemaphoreCreateBinary();
    if (!s_bind_sem) {
        ESP_LOGE(TAG, "sem alloc failed -- bindings will not be created");
        return;
    }
    zhac_adapter_register_configure(&cfg_bind, &cfg_report);
    zhac_adapter_register_configure_ex(&cfg_cmd, &cfg_read, &cfg_sleep);
    zhac_adapter_register_configure_write(&cfg_write);
    ESP_LOGI(TAG, "configure transports registered (bind/report/cmd/read/write)");
}

// ZDO bind/unbind for the SPA's Bind tab. Same transport as cfg_bind, but the
// destination is caller-chosen rather than always the coordinator.
bool esp_zb_zdo_bind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                     uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep,
                     bool unbind) {
    return do_bind(src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep, unbind);
}

#endif  // CONFIG_ZHAC_ESP_ZIGBEE
