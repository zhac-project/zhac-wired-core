// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_groups -- native ZCL Groups (cluster 0x0004) for the esp-zigbee backend.
//
// WHAT THIS IS FOR
// ----------------
// A device that is a member of group N obeys commands addressed to N, including
// groupcasts a hardware zone-remote emits directly. That is the point: the
// remote talks to the bulbs without the gateway in the path, so it keeps working
// when ZHAC is rebooting. This is NOT the "Collections" feature, which is
// gateway-side fan-out (one command re-sent per member) and lives in
// groups_store.cpp.
//
// These three functions are declared in zhac-components' zigbee_mgr.h but
// implemented there only against the ZNP transport, in sources this SKU does not
// compile. Same declarations, same semantics, esp-zigbee transport.
//
// Add/Remove are delivery-confirmed only -- the device's Add Group Response
// carries a status we do not wait for, exactly as the ZNP path behaves. The
// authoritative answer is always a Get Group Membership readback, which is why
// the SPA has a "Refresh from device" button.
#include "sdkconfig.h"

#if CONFIG_ZHAC_ESP_ZIGBEE

#include "esp_zigbee_backend.h"
#include "esp_zb_interview.h"   // esp_zb_af_send
#include "esp_zb_lock.h"
#include "esp_zb_zcl_frame.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "zcl_seq.h"
#include "zigbee_mgr.h"

#include <cstring>

static const char* TAG = "esp_zb_grp";

namespace {

constexpr uint16_t kGroupsCluster = 0x0004;
constexpr uint8_t  kCmdAddGroup   = 0x00;
constexpr uint8_t  kCmdGetMember  = 0x02;
constexpr uint8_t  kCmdRemoveGrp  = 0x03;

constexpr uint32_t kMemberTimeoutMs = 5000;

// Readback slot. One membership query at a time, from request to answer:
// local and cloud callers run on different tasks and used to take each
// other's answers (review F13). The answer must come from the device and
// endpoint asked, carry the request's TSN and travel server -> client.
SemaphoreHandle_t s_member_sem = nullptr;
SemaphoreHandle_t s_member_op  = nullptr;
volatile uint16_t s_member_nwk = 0;      // 0 = not waiting
uint8_t           s_member_ep  = 0;
uint8_t           s_member_tsn = 0;
uint8_t           s_member_buf[64];
uint8_t           s_member_len = 0;

SemaphoreHandle_t member_sem() {
    if (!s_member_sem) s_member_sem = xSemaphoreCreateBinary();
    if (!s_member_op)  s_member_op  = xSemaphoreCreateMutex();
    return s_member_sem && s_member_op ? s_member_sem : nullptr;
}

}  // namespace

// Add Group: group id LE(2) + group name as a ZCL string (length byte 0).
// The trailing zero-length name is REQUIRED by the spec; omitting it makes
// conformant devices reject the frame as malformed.
bool zigbee_zcl_group_add(uint16_t nwk_addr, uint8_t ep, uint16_t group_id) {
    // Whatever group a device joins, the coordinator joins too, so commands
    // the device (or a remote) sends to that group are heard here.
    esp_zb_coordinator_join_group(group_id);
    const uint8_t payload[3] = {
        static_cast<uint8_t>(group_id & 0xFF),
        static_cast<uint8_t>((group_id >> 8) & 0xFF),
        0x00,
    };
    uint8_t f[32];
    const size_t n = zhc_zcl::cluster_command(f, sizeof(f), zcl_seq_next(),
                                              kCmdAddGroup, payload,
                                              sizeof(payload), /*flags=*/0);
    if (n == 0) return false;
    const bool ok = esp_zb_af_send(nwk_addr, ep ? ep : 1, kGroupsCluster, f, n);
    ESP_LOGI(TAG, "group_add nwk 0x%04x ep %u gid %u: %s",
             nwk_addr, ep, group_id, ok ? "sent" : "FAILED");
    return ok;
}

// Remove Group: group id LE(2). No name field here.
bool zigbee_zcl_group_remove(uint16_t nwk_addr, uint8_t ep, uint16_t group_id) {
    const uint8_t payload[2] = {
        static_cast<uint8_t>(group_id & 0xFF),
        static_cast<uint8_t>((group_id >> 8) & 0xFF),
    };
    uint8_t f[32];
    const size_t n = zhc_zcl::cluster_command(f, sizeof(f), zcl_seq_next(),
                                              kCmdRemoveGrp, payload,
                                              sizeof(payload), /*flags=*/0);
    if (n == 0) return false;
    const bool ok = esp_zb_af_send(nwk_addr, ep ? ep : 1, kGroupsCluster, f, n);
    ESP_LOGI(TAG, "group_remove nwk 0x%04x ep %u gid %u: %s",
             nwk_addr, ep, group_id, ok ? "sent" : "FAILED");
    return ok;
}

// Get Group Membership with a group count of 0, which the spec defines as
// "report every group you are in". Blocks for the response.
//
// Returns true iff a response arrived; *out_count may legitimately be 0, which
// means "device is in no groups" -- distinct from a false return, which means
// "device did not answer". The caller must not conflate them: reconciling the
// mirror to empty on a timeout would silently wipe a working configuration.
bool zigbee_zcl_get_group_membership(uint16_t nwk_addr, uint8_t ep,
                                     uint16_t* out_gids, uint8_t max,
                                     uint8_t* out_count) {
    if (out_count) *out_count = 0;
    if (!out_gids || max == 0 || !out_count) return false;
    SemaphoreHandle_t sem = member_sem();
    if (!sem) return false;
    if (xSemaphoreTake(s_member_op, pdMS_TO_TICKS(2 * kMemberTimeoutMs)) != pdTRUE) {
        ESP_LOGW(TAG, "membership query nwk 0x%04x: another query did not finish", nwk_addr);
        return false;
    }

    const uint8_t tsn    = zcl_seq_next();
    const uint8_t dst_ep = ep ? ep : 1;
    const uint8_t payload[1] = {0x00};   // group count 0 = all
    uint8_t f[32];
    const size_t n = zhc_zcl::cluster_command(f, sizeof(f), tsn, kCmdGetMember,
                                              payload, sizeof(payload), /*flags=*/0);
    bool sent = false;
    if (n) {
        // Arm under the stack lock: answers are fed on the stack task with it
        // held, so none can land between draining and arming.
        zb_lock::Guard g;
        if (g) {
            xSemaphoreTake(sem, 0);          // drain a stale post
            s_member_len = 0;
            s_member_ep  = dst_ep;
            s_member_tsn = tsn;
            s_member_nwk = nwk_addr;
            sent = esp_zb_af_send(nwk_addr, dst_ep, kGroupsCluster, f, n);
            if (!sent) s_member_nwk = 0;
        }
    }
    const bool got = sent && xSemaphoreTake(sem, pdMS_TO_TICKS(kMemberTimeoutMs)) == pdTRUE;
    { zb_lock::Guard g; s_member_nwk = 0; }   // a late answer is nobody's

    // A truncated answer is not "in fewer groups": it must leave the mirror
    // as it is (review F14).
    const bool ok = got && zhc_zcl::parse_group_membership(s_member_buf, s_member_len,
                                                           out_gids, max, out_count);
    xSemaphoreGive(s_member_op);
    if (!got)     ESP_LOGW(TAG, "membership query nwk 0x%04x ep %u: no response", nwk_addr, dst_ep);
    else if (!ok) ESP_LOGW(TAG, "membership query nwk 0x%04x ep %u: malformed response", nwk_addr, dst_ep);
    else          ESP_LOGI(TAG, "membership nwk 0x%04x ep %u: %u group(s)", nwk_addr, dst_ep, *out_count);
    return ok;
}

// Fed every inbound ZCL frame by the APS hook. Consumes only the Get Group
// Membership Response we are actively waiting for.
bool zb_groups_feed_zcl(uint16_t nwk, uint16_t cluster_id, uint8_t src_ep,
                        const uint8_t* zcl, uint8_t zcl_len) {
    if (cluster_id != kGroupsCluster || s_member_nwk == 0 || s_member_nwk != nwk ||
        src_ep != s_member_ep) return false;
    zhc_zcl::Header h;
    if (!zhc_zcl::parse_header(zcl, zcl_len, &h) || !h.cluster_specific ||
        !(h.fc & 0x08) ||                                   // server -> client
        h.cmd != kCmdGetMember || h.tsn != s_member_tsn) return false;

    // Copy the body only (past the header) so the parser indexes from 0.
    s_member_len = static_cast<uint8_t>(zcl_len - h.hdr_len);
    if (s_member_len > sizeof(s_member_buf)) s_member_len = sizeof(s_member_buf);
    std::memcpy(s_member_buf, zcl + h.hdr_len, s_member_len);
    s_member_nwk = 0;                    // answered: nothing may overwrite it now
    xSemaphoreGive(s_member_sem);
    return true;
}

#endif  // CONFIG_ZHAC_ESP_ZIGBEE
