// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_ota -- the stack's OTA Upgrade server, for a Zigbee device firmware update (research 2026-11-18 C2).
//
// Endpoint 1 declares the server (esp_zigbee_backend.cpp) and the APS hook lets cluster 0x0019 through, so the
// stack answers every device's Query Next Image, "no image" while none is loaded. main/device_ota.cpp loads one
// image for one device and decides here, on the stack task, who gets it (esp-zigbee-lib 2.x: the server only
// serves files; START, SENDING and FINISH of its progress callback replace the 1.x query and status callbacks).
#include <cstdint>

#include "esp_log.h"
#include "esp_zb_lock.h"
#include "esp_zigbee_backend.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"
#include "ezbee/zcl/zcl_core.h"

static const char* TAG = "zb_ota";
static constexpr uint8_t kEp = 1;

static esp_zb_ota_event_fn s_on_event = nullptr;
static esp_zb_ota_frame_fn s_on_frame = nullptr;
static ezb_zcl_ota_file_handle_t s_file = EZB_ZCL_OTA_FILE_INVALID_HANDLE;

// On the stack task only (its address map).
static uint64_t ieee_of_short(uint16_t nwk) {
    ezb_extaddr_t ext{};
    return ezb_address_extended_by_short(nwk, &ext) == 0 ? ext.u64 : 0;
}

static uint64_t ieee_of(const ezb_address_t& a) {
    if (a.addr_mode == EZB_ADDR_MODE_EXT) return a.u.extended_addr.u64;
    return a.addr_mode == EZB_ADDR_MODE_SHORT ? ieee_of_short(a.u.short_addr) : 0;
}

static bool tell(int kind, uint64_t ieee, uint16_t manuf, uint16_t type, uint32_t a) {
    return s_on_event && s_on_event(kind, ieee, manuf, type, a);
}

// The query that matched the image (START) is answered "no image" unless the app takes it; blocks and the Upgrade
// End of any device but the one being updated are aborted, so a device cannot pull the image without being served.
static void on_progress(ezb_zcl_ota_upgrade_server_progress_message_t* m) {
    const uint64_t from = m->in.header ? ieee_of(m->in.header->src_addr) : 0;
    bool go = true;
    switch (m->in.progress) {
        case EZB_ZCL_OTA_UPGRADE_PROGRESS_START: {
            const auto& q = m->in.start.query;
            go = tell(ZB_OTA_START, from ? from : ieee_of_short(q.short_address), q.manuf_code, q.image_type,
                      q.file_version);
            m->out.result = go ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE;
            return;
        }
        case EZB_ZCL_OTA_UPGRADE_PROGRESS_SENDING:
            go = tell(ZB_OTA_BLOCK, from, 0, 0, m->in.sending.file_offset);
            break;
        case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH:
            go = tell(ZB_OTA_END, from, 0, 0, m->in.finish.status);
            m->out.finish.current_time = 0;
            m->out.finish.upgrade_time = 0;   // apply at once
            break;
        case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT:
            tell(ZB_OTA_ABORT, from, 0, 0, 0);
            break;
        default:
            break;
    }
    m->out.result = go ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_ABORT;
}

static void on_zcl_action(ezb_zcl_core_action_callback_id_t id, void* msg) {
    if (id == EZB_ZCL_CORE_OTA_UPGRADE_SERVER_PROGRESS_CB_ID && msg)
        on_progress(static_cast<ezb_zcl_ota_upgrade_server_progress_message_t*>(msg));
}

void esp_zb_ota_attach(void) { ezb_zcl_core_action_handler_register(on_zcl_action); }

void esp_zb_ota_on_frame(const ezb_apsde_data_ind_t* ind) {
    if (s_on_frame && ind->asdu && ind->asdu_length) s_on_frame(ieee_of(ind->src_address), ind->asdu, ind->asdu_length);
}

void esp_zb_ota_handlers(esp_zb_ota_event_fn on_event, esp_zb_ota_frame_fn on_frame) {
    s_on_event = on_event;
    s_on_frame = on_frame;
}

bool esp_zb_ota_load(const uint8_t* image, uint32_t size) {
    if (!esp_zigbee_backend_network_up()) return false;
    zb_lock::Guard g;
    if (!g || s_file != EZB_ZCL_OTA_FILE_INVALID_HANDLE) return false;
    s_file = ezb_zcl_ota_upgrade_add_ota_file(kEp, image, size);
    if (s_file == EZB_ZCL_OTA_FILE_INVALID_HANDLE) ESP_LOGW(TAG, "the stack refused the image (%u bytes)", (unsigned)size);
    return s_file != EZB_ZCL_OTA_FILE_INVALID_HANDLE;
}

bool esp_zb_ota_unload(void) {
    if (s_file == EZB_ZCL_OTA_FILE_INVALID_HANDLE) return true;
    zb_lock::Guard g;
    if (!g) return false;   // still loaded: the caller must keep the image
    ezb_zcl_ota_upgrade_remove_ota_file(kEp, s_file);
    s_file = EZB_ZCL_OTA_FILE_INVALID_HANDLE;
    return true;
}

bool esp_zb_ota_notify(uint16_t nwk) {
    if (!esp_zigbee_backend_network_up()) return false;
    zb_lock::Guard g;
    if (!g) return false;
    ezb_zcl_ota_upgrade_image_notify_cmd_t cmd{};
    cmd.cmd_ctrl.dst_ep = 0xFF;   // the device's OTA client, wherever it is
    cmd.cmd_ctrl.src_ep = kEp;
    cmd.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    cmd.cmd_ctrl.dst_addr.u.short_addr = nwk;
    // Query jitter 100, no image fields (as z2m sends it): the device asks at once, whatever image is loaded.
    cmd.payload.type = EZB_ZCL_OTA_UPGRADE_IMAGE_NOTIFY_PAYLOAD_TYPE_JITTER;
    cmd.payload.query_jitter = 100;
    return ezb_zcl_ota_upgrade_image_notify_cmd_req(&cmd) == 0;
}
