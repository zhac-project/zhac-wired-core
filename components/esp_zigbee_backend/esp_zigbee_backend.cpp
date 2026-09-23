// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_backend -- esp-zigbee-lib (v2.x) as ZHAC's radio layer.
//
// THE LOAD-BEARING IDEA
// ---------------------
// ZHAC's product is "dumb stack on the radio, device intelligence on the host":
// 6,235 device definitions decode raw ZCL in embedded-zhc. That only works if
// something hands us the raw APS payload. esp-zigbee-lib does, via
// ezb_apsde_data_indication_handler_register() -- the callback receives the
// ASDU bytes plus src address, endpoints, cluster, profile and LQI, and its
// return value decides whether the stack also processes the frame.
//
// That callback maps onto zhac_adapter_try_decode() parameter-for-parameter,
// which is why this backend is thin. Returning false lets the stack continue
// handling the frame normally (ZDO, ZCL foundation) even when we decoded it --
// deliberately conservative: ZHAC observes, it does not swallow.
//
// BOTH SKUs, ONE FILE
// -------------------
// The only difference is where the PHY is, and that is a config field:
//   esp32s31 -> ESP_ZIGBEE_RADIO_MODE_NATIVE    (own 802.15.4)
//   esp32p4  -> ESP_ZIGBEE_RADIO_MODE_UART_RCP  (C6 running stock ot_rcp)
// Everything above is identical, which was the entire argument for keeping
// both SKUs in one repo.
#include "esp_zigbee_backend.h"
#include "esp_zb_interview.h"
#include <ctime>
#include "sdkconfig.h"

#if CONFIG_ZHAC_ESP_ZIGBEE

#include "device_backend.h"
#include "esp_attr.h"     // __NOINIT_ATTR
#include "esp_log.h"
#include "esp_system.h"   // esp_restart
#include "esp_timer.h"
#include "event_bus.h"
#include "esp_zb_lock.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/error.h"
#include "ezbee/af.h"
#include "zigbee_mgr.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/groups.h"
#include "ezbee/zcl/cluster/groups_desc.h"
#include "ezbee/platform/radio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zhac_task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "task_stacks.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "zigbee_diagnostics.h"
#include "zigbee_configure_queue.h"
#include "zigbee_identity.h"
#include "zigbee_pool.h"
#include "zcl_seq.h"

// Defined in zhac-components' zhc_shadow_bridge.cpp; declared in no header.
extern "C" void zhc_shadow_bridge_register(void);

#include <cstring>

static const char* TAG = "esp_zb";

static bool s_running = false;
// Set from on_app_signal: true once a PAN exists (formed now, or resumed from
// the dataset). Opening the network before this fails inside the stack.
static bool s_formed  = false;

// Defined below, used by zb_init() above its definition.
static bool zb_network_ready();

// A mainloop exit below this uptime is treated as a startup failure and does
// NOT trigger a reboot -- see task_zigbee().
static constexpr int kMainloopRebootMinUptimeS = 60;

// ── Raw APS ingress: the make-or-break path (gate G2) ────────────────────
//
// Every inbound application frame lands here with its ASDU intact. We look the
// device up in the pool for the identity strings the definition matcher needs
// (modelId + manufacturerName), tell the adapter the (ieee, nwk) tuple so
// converters that reply have a destination, then decode.
// zigbee_identity_on_af_incoming() takes a ZNP AF_INCOMING_MSG payload, not a
// raw ASDU -- it reads cluster at [2], src nwk at [4], data_len at [16], body
// from [17]. Everything BEHIND that entry point (the worker task, pool update,
// re-match, persist) is transport-neutral and well tested, so rather than
// duplicate that logic we hand it a synthetic header.
//
// Only the four fields it actually reads are filled; the rest stay zero. If
// that layout ever changes upstream this breaks silently, which is why the
// better fix is a transport-neutral entry point in zhac-components -- raised,
// not done here, because this SKU cannot modify that repo.
static void feed_late_identity(uint16_t nwk, uint16_t cluster_id,
                               const uint8_t* zcl, uint16_t zcl_len) {
    if (cluster_id != 0x0000 || !zcl || zcl_len == 0) return;
    if (zcl_len > 200) return;                 // keep the frame off the stack

    uint8_t af[17 + 200] = {};
    af[2]  = static_cast<uint8_t>(cluster_id & 0xFF);
    af[3]  = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af[4]  = static_cast<uint8_t>(nwk & 0xFF);
    af[5]  = static_cast<uint8_t>((nwk >> 8) & 0xFF);
    af[16] = static_cast<uint8_t>(zcl_len);
    std::memcpy(af + 17, zcl, zcl_len);
    zigbee_identity_on_af_incoming(af, static_cast<uint8_t>(17 + zcl_len));
}

// Tuya end-devices (MiBoxer FUT089Z, TS0044, TS011F, TS0001, ...) read genTime
// attribute 0x0007 (LocalTime) from the coordinator right after joining and
// keep asking until they get a real value; the remotes gate their button
// reporting on it. Nothing on our endpoint serves the Time cluster, so answer
// here the way the ZNP path's zigbee_respond_gen_time does: UTC seconds since
// 2000 once the clock is synced, UNSUPPORTED_ATTRIBUTE for everything else
// (and for the time itself before NTP) so the device stops probing.
static void respond_gen_time(uint16_t nwk, uint8_t dst_ep, const uint8_t* zcl, uint16_t len) {
    if (nwk == 0 || !zcl || len < 3) return;
    const bool   mfg = (zcl[0] & 0x04) != 0;
    const size_t hdr = mfg ? 5 : 3;
    if (len < hdr || (zcl[0] & 0x03) != 0x00) return;   // profile-wide frames only
    if (zcl[hdr - 1] != 0x00) return;                      // Read Attributes
    const uint8_t tsn = zcl[hdr - 2];
    constexpr uint32_t kEpoch2000 = 946684800u;             // 1970 -> 2000 in seconds
    const uint32_t now_unix = static_cast<uint32_t>(time(nullptr));
    const bool     clock_ok = now_unix > kEpoch2000;
    const uint32_t utc_2000 = clock_ok ? now_unix - kEpoch2000 : 0;
    uint8_t body[80];
    body[0] = 0x18;   // profile-wide, server -> client, default response off
    body[1] = tsn;
    body[2] = 0x01;   // Read Attributes Response
    size_t n = 3;
    for (size_t i = hdr; i + 1 < len && n + 8 <= sizeof(body); i += 2) {
        const uint16_t attr = static_cast<uint16_t>(zcl[i] | (static_cast<uint16_t>(zcl[i + 1]) << 8));
        body[n++] = zcl[i];
        body[n++] = zcl[i + 1];
        if ((attr == 0x0007 || attr == 0x0000) && clock_ok) {   // LocalTime / Time
            body[n++] = 0x00;   // SUCCESS
            body[n++] = 0xE2;   // UTC time
            body[n++] = static_cast<uint8_t>(utc_2000);
            body[n++] = static_cast<uint8_t>(utc_2000 >> 8);
            body[n++] = static_cast<uint8_t>(utc_2000 >> 16);
            body[n++] = static_cast<uint8_t>(utc_2000 >> 24);
        } else {
            body[n++] = 0x86;   // UNSUPPORTED_ATTRIBUTE
        }
    }
    const bool sent = esp_zb_af_send(nwk, dst_ep, 0x000A, body, n);
    ESP_LOGI(TAG, "genTime read-resp -> nwk 0x%04x ep %u utc2000=%lu%s%s", nwk, dst_ep,
             static_cast<unsigned long>(utc_2000),
             clock_ok ? "" : " (clock not synced: UNSUPPORTED)", sent ? "" : " SEND FAILED");
}

// Does this device switch its radio off between polls? The adapter holds
// writes and Tuya queries for such devices and resends them when they next
// transmit (zhac_adapter_register_sleepy). For the coordinator's own children
// the neighbour table knows (rx_on_when_idle). A device parented by a router
// is not in it; there a battery power source (ZCL powerSource 0x03, bit 7 =
// backup battery) stands in.
static bool esp_zb_is_sleepy(uint64_t ieee) {
    {
        zb_lock::Guard g;
        if (g) {
            ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
            ezb_nwk_neighbor_info_t nb{};
            while (ezb_nwk_get_next_neighbor(&it, &nb) == EZB_ERR_NONE) {
                if (nb.ieee_addr.u64 == ieee) return nb.rx_on_when_idle == 0;
            }
        }
    }
    ZapDevice d{};
    return zigbee_pool_snapshot(ieee, &d) && (d.power_source & 0x7F) == 0x03;
}

// Short addresses get reused. The pool learns them from announces, the stack's
// own address map from every frame -- the map is authoritative.
static void pool_set_nwk(uint64_t ieee, uint16_t nwk) {
    zigbee_pool_with_device(ieee, [](ZapDevice* d, void* c) {
        d->nwk_addr = *static_cast<uint16_t*>(c);
    }, &nwk);
    zigbee_pool_mark_dirty();
}

// ── Coordinator endpoint + group membership ─────────────────────────────
// A hardware zone-remote (MiBoxer FUT089Z: zones -> groups 101..108) is
// groupcast-only and not bindable, so its presses reach this coordinator only
// while endpoint 1 is a member of those groups. The stack's Groups server on
// EP1 keeps the APS group table; a self-addressed Add Group is the public way
// to fill it. The TI ZNP path has no equivalent (NATIVE_ZCL_GROUPS_DESIGN.md,
// Part B) -- this is the esp-zigbee "rank 1" path from that document.
static constexpr uint16_t kDefaultGroups[] = {101, 102, 103, 104, 105, 106, 107, 108};

static void register_coordinator_endpoint() {
    ezb_af_ep_config_t cfg{};
    cfg.ep_id              = 1;
    cfg.app_profile_id     = EZB_AF_HA_PROFILE_ID;
    cfg.app_device_id      = 0x0005;   // configuration tool, as the ZNP registers
    cfg.app_device_version = 0;
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_af_ep_desc_t     ep  = ezb_af_create_endpoint_desc(&cfg);
    if (dev == EZB_INVALID_AF_DEVICE_DESC || ep == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGW(TAG, "coordinator endpoint: descriptor alloc failed -- no group membership");
        return;
    }
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_basic_create_cluster_desc(nullptr, EZB_ZCL_CLUSTER_SERVER));
    // Server keeps the APS group table; client is what lets EP1 SEND Add Group
    // (the request is refused without it). One descriptor per role: a combined
    // role mask with a NULL config faulted inside the library on the bench.
    ezb_zcl_groups_cluster_server_config_t grp_cfg{};
    grp_cfg.name_support = 0;   // no group names
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_groups_create_cluster_desc(&grp_cfg, EZB_ZCL_CLUSTER_SERVER));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_groups_create_cluster_desc(nullptr, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_device_add_endpoint_desc(dev, ep);
    const ezb_err_t e = ezb_af_device_desc_register(dev);
    ESP_LOGI(TAG, "coordinator endpoint 1 (Basic + Groups server): %s (%d)",
             e == 0 ? "registered" : "FAILED", (int)e);
}

// The stack's own Groups server calls this to add an endpoint to a group (it
// is what an incoming Add Group ends in): exported by libesp-zigbee-core but
// not in any header. Argument order read from the library's call site: the
// group id, then the endpoint (checked against 1..254 in the prologue). A
// self-addressed ZCL Add Group does not loop back on this stack, so this is
// the one working way to make endpoint 1 a group member.
extern "C" int aps_group_table_add(uint16_t group_id, uint8_t endpoint);

bool esp_zb_coordinator_join_group(uint16_t group_id) {
    zb_lock::Guard g;
    if (!g) return false;
    const int e = aps_group_table_add(group_id, 1);
    if (e != 0) ESP_LOGW(TAG, "coordinator join group %u: aps_group_table_add -> %d", group_id, e);
    return e == 0;
}

static void join_default_groups() {
    unsigned ok = 0;
    for (uint16_t g : kDefaultGroups) ok += esp_zb_coordinator_join_group(g) ? 1 : 0;
    ESP_LOGI(TAG, "coordinator endpoint 1 joined %u/%u groups (%u..%u, zone remotes)", ok,
             (unsigned)(sizeof(kDefaultGroups) / sizeof(kDefaultGroups[0])),
             kDefaultGroups[0], kDefaultGroups[sizeof(kDefaultGroups) / sizeof(kDefaultGroups[0]) - 1]);
}


// The joins and the read-back run on their own task: app_main's stack is
// 4 KB and the ZCL request builder plus a blocking membership query overran
// it (FreeRTOS assert in the stdout mutex right after "joining groups").
static void group_join_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the stack settle after formation
    join_default_groups();
    vTaskDelete(nullptr);
}
static void spawn_group_join_task() {
    if (xTaskCreate(group_join_task, "zb_groups", 6144, nullptr, 3, nullptr) != pdPASS)
        ESP_LOGW(TAG, "group join task create failed -- zone remotes will not be heard");
}

// ── ZCL Default Response + retry guard ─────────────────────────────────
// Tuya sleepy remotes (TS0044, TS004F, FUT089Z) resend a command when no ZCL
// Default Response comes back -- on the bench one press of a TS0044 arrived
// as two "1_single" actions 300 ms apart and toggled a relay twice. The ZNP
// path answers every unicast command (zigbee_send_default_response); this
// backend consumed all ZCL frames and answered nothing. Same rule here: reply
// to every unicast command whose frame control did not opt out, never to a
// response, and drop an identical resend that slips through anyway.
static bool global_cmd_wants_default_response(uint8_t cmd) {
    switch (cmd) {   // global responses, never ACKed (ZCL 2.5.12)
        case 0x01: case 0x04: case 0x05: case 0x07: case 0x09:
        case 0x0B: case 0x0D: case 0x10: case 0x12: case 0x14: case 0x16: return false;
        default: return true;
    }
}

static void send_default_response_if_needed(uint16_t nwk, uint8_t src_ep, uint16_t cluster,
                                            const uint8_t* zcl, uint16_t len) {
    if (nwk == 0 || !zcl || len < 3) return;
    const uint8_t in_fc = zcl[0];
    const bool    mfg   = (in_fc & 0x04) != 0;
    const size_t  hdr   = mfg ? 5 : 3;
    if (len < hdr) return;
    const bool    is_cs = (in_fc & 0x03) == 0x01;
    const uint8_t tsn   = zcl[hdr - 2];
    const uint8_t cmd   = zcl[hdr - 1];
    if (in_fc & 0x10) return;                                   // opted out
    if (!is_cs && (cmd == 0x0B || !global_cmd_wants_default_response(cmd))) return;
    if (!is_cs && cmd == 0x00 && cluster == 0x000A) return;     // genTime read: answered in full
    uint8_t out[8];
    size_t  n  = 0;
    uint8_t fc = 0x10;                                          // no response to this response
    if (!(in_fc & 0x08)) fc |= 0x08;                            // flip direction
    if (mfg) fc |= 0x04;
    out[n++] = fc;
    if (mfg) { out[n++] = zcl[1]; out[n++] = zcl[2]; }
    out[n++] = tsn;
    out[n++] = 0x0B;                                            // Default Response
    out[n++] = cmd;
    out[n++] = 0x00;                                            // SUCCESS
    esp_zb_af_send(nwk, src_ep ? src_ep : 1, cluster, out, n);
}

// An identical cluster-specific command (same source, cluster, TSN) inside
// 1.5 s is the device's retry, not a second press.
static bool zcl_is_retry(uint16_t nwk, uint16_t cluster, const uint8_t* zcl, uint16_t len) {
    if (len < 3 || (zcl[0] & 0x03) != 0x01) return false;       // commands only
    const uint8_t tsn = (zcl[0] & 0x04) ? zcl[3] : zcl[1];
    struct Seen { uint16_t nwk, cluster; uint8_t tsn; int64_t us; };
    static Seen s_seen[8];
    static uint8_t s_next = 0;
    const int64_t now = esp_timer_get_time();
    for (const Seen& e : s_seen) {
        if (e.nwk == nwk && e.cluster == cluster && e.tsn == tsn && now - e.us < 1500 * 1000LL) return true;
    }
    s_seen[s_next] = Seen{nwk, cluster, tsn, now};
    s_next = static_cast<uint8_t>((s_next + 1) % 8);
    return false;
}

static bool on_apsde_indication_inner(const ezb_apsde_data_ind_t* ind) {
    if (!ind || !ind->asdu || ind->asdu_length == 0) return false;
    // ZDO answers (profile 0x0000: Bind_rsp 0x8021, Simple_Desc_rsp 0x8004, ...)
    // reach this hook too. The interview engine gets them through its ZDO
    // callbacks; feeding them to the ZCL decoder only produced
    // "decode_frame failed cluster=0x8021" noise and fake unhandled entries.
    if (ind->profile_id == 0x0000) return false;
    // Groupcast destination: the decoder synthesises `zone` from it (FUT089Z 101..108).
    const uint16_t group_id = (ind->dst_address.addr_mode == EZB_ADDR_MODE_GROUP)
                                  ? ind->dst_address.u.group_addr.group : 0;
    if (group_id) {
        // Rare and diagnostic: proves the coordinator's group membership works.
        ESP_LOGI(TAG, "groupcast to group %u: cluster 0x%04x from %s 0x%04x, %u bytes", group_id,
                 ind->cluster_id, ind->src_address.addr_mode == EZB_ADDR_MODE_EXT ? "ieee" : "nwk",
                 ind->src_address.addr_mode == EZB_ADDR_MODE_EXT
                     ? (unsigned)(ind->src_address.u.extended_addr.u64 & 0xFFFF)
                     : (unsigned)ind->src_address.u.short_addr,
                 (unsigned)ind->asdu_length);
    }

    // Source may arrive short or extended; the pool is keyed by IEEE.
    uint64_t ieee = 0;
    uint16_t nwk  = 0;
    if (ind->src_address.addr_mode == EZB_ADDR_MODE_EXT) {
        // ezb_extaddr_t is a packed union struct, not a raw byte array.
        ieee = ind->src_address.u.extended_addr.u64;
        ZapDevice snap{};
        if (zigbee_pool_snapshot(ieee, &snap)) nwk = snap.nwk_addr;
    } else {
        nwk = ind->src_address.u.short_addr;
        ZapDevice snap{};
        if (zigbee_pool_snapshot_by_nwk(nwk, &snap)) {
            ieee = snap.ieee_addr;
            // A stale pool entry can still hold the address a newly joined
            // device now uses; every frame from the newcomer would then decode
            // under the old device and the newcomer would look silent.
            ezb_extaddr_t ext{};
            if (ezb_address_extended_by_short(nwk, &ext) == 0 && ext.u64 != 0 &&
                ext.u64 != ieee) {
                pool_set_nwk(ieee, 0);   // the old holder no longer has it
                ZapDevice real{};
                if (zigbee_pool_snapshot(ext.u64, &real)) {
                    pool_set_nwk(ext.u64, nwk);
                    ESP_LOGW(TAG, "nwk 0x%04x is %016llx, not %016llx -- pool refreshed",
                             nwk, (unsigned long long)ext.u64, (unsigned long long)ieee);
                    ieee = ext.u64;
                } else {
                    ESP_LOGW(TAG, "nwk 0x%04x is %016llx (not in the pool), not %016llx "
                                  "-- stale address cleared", nwk,
                             (unsigned long long)ext.u64, (unsigned long long)ieee);
                    ieee = 0;
                }
            }
        } else {
            // Short address the pool does not know: a sleepy device rejoined
            // through a router without an announce we saw, or came back under
            // a new address. The stack's address map knows the IEEE; refresh
            // the pool so the frame decodes under the right device instead of
            // vanishing into the unhandled counter.
            ezb_extaddr_t ext{};
            if (ezb_address_extended_by_short(nwk, &ext) == 0 && ext.u64 != 0 &&
                !zigbee_pool_snapshot(ext.u64, &snap)) {
                // Joined (the stack resolves it) but not ours: adopt it. The
                // pool entry appears now, so this frame and the next ones
                // decode under the right device; the interview fills identity.
                zb_interview_adopt(ext.u64, nwk);
                zigbee_pool_snapshot(ext.u64, &snap);
                ieee = snap.ieee_addr;
            } else if (ezb_address_extended_by_short(nwk, &ext) == 0 && ext.u64 != 0 &&
                       zigbee_pool_snapshot(ext.u64, &snap)) {
                ieee = ext.u64;
                pool_set_nwk(ieee, nwk);
                ESP_LOGI(TAG, "%016llx now at nwk 0x%04x (pool had 0x%04x) -- address refreshed",
                         (unsigned long long)ieee, nwk, snap.nwk_addr);
            }
        }
    }

    if (ind->cluster_id == 0x000A) {
        respond_gen_time(nwk, ind->src_endpoint, ind->asdu, ind->asdu_length);
    }
    if (group_id == 0) {
        send_default_response_if_needed(nwk, ind->src_endpoint, ind->cluster_id, ind->asdu, ind->asdu_length);
        if (zcl_is_retry(nwk, ind->cluster_id, ind->asdu, ind->asdu_length)) {
            ESP_LOGD(TAG, "retry of a command from nwk 0x%04x cluster 0x%04x dropped", nwk, ind->cluster_id);
            return false;
        }
    }

    if (ieee == 0) {
        // Frame from a device not in the pool: nothing to match a definition
        // against yet. Record it so /api/diagnostics/unhandled shows it rather
        // than it vanishing. ZCL byte 0 is frame control (bit0 = cluster
        // specific); the attr/cmd id sits after the 1-byte TSN.
        const bool cluster_specific = (ind->asdu[0] & 0x01) != 0;
        const uint16_t attr_or_cmd = (ind->asdu_length >= 3) ? ind->asdu[2] : 0;
        zb_diag_record_unhandled(ind->cluster_id, attr_or_cmd, cluster_specific, 0);
        // Say so, rate-limited: a device that talks but is not in the pool was
        // invisible in the log, which reads exactly like a device that is silent.
        static int64_t s_unknown_log_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_unknown_log_us > 10 * 1000000LL) {
            s_unknown_log_us = now_us;
            ESP_LOGI(TAG, "frame from a source not in the pool: nwk 0x%04x cluster 0x%04x %s 0x%02x",
                     nwk, ind->cluster_id, cluster_specific ? "cmd" : "attr", attr_or_cmd);
        }
        return false;
    }

    // Link quality. Every APS indication carries it and nothing else on this
    // backend ever wrote it, so the UI showed lqi 0 for every device forever.
    // Cheap in-place update under the pool's own visitor lock.
    if (ieee != 0 && ind->lqi != 0) {
        struct LqiCtx { uint8_t lqi; uint32_t now; };
        LqiCtx lc{ind->lqi, esp_zigbee_backend_wall_clock_s()};
        zigbee_pool_with_device(ieee, [](ZapDevice* d, void* c) {
            auto* x = static_cast<LqiCtx*>(c);
            d->link_quality = x->lqi;
            if (x->now) d->last_seen = x->now;
        }, &lc);
    }

    // Give the interview engine first sight of the frame. It only consumes
    // Basic-cluster replies it is actively waiting for; everything continues
    // to the adapter either way.
    zb_interview_note_traffic(ieee);   // awake now: retry the interview at once
    zb_interview_feed_zcl(nwk, ind->cluster_id, ind->src_endpoint,
                          ind->asdu, static_cast<uint8_t>(ind->asdu_length));
    zb_groups_feed_zcl(nwk, ind->cluster_id, ind->asdu,
                       static_cast<uint8_t>(ind->asdu_length));
    feed_late_identity(nwk, ind->cluster_id, ind->asdu, ind->asdu_length);

    // Identity strings for the matcher, straight from the pool snapshot.
    ZapDevice dev{};
    const bool have = zigbee_pool_snapshot(ieee, &dev);
    const char* model = (have && dev.model_id[0]) ? dev.model_id : nullptr;
    const char* manuf = (have && dev.manufacturer_name[0]) ? dev.manufacturer_name : nullptr;

    zhac_adapter_set_runtime_addr(ieee, nwk);

    const bool decoded = zhac_adapter_try_decode(
        ieee, model, manuf,
        group_id,
        ind->cluster_id, ind->src_endpoint, ind->lqi,
        ind->asdu, ind->asdu_length);

    if (!decoded) {
        const bool cluster_specific = (ind->asdu[0] & 0x01) != 0;
        const uint16_t attr_or_cmd = (ind->asdu_length >= 3) ? ind->asdu[2] : 0;
        zb_diag_record_unhandled(ind->cluster_id, attr_or_cmd, cluster_specific, ieee);
    }

    // The decoder found a definition for this device, so its identity is
    // complete and supported whatever the pool says. A pool entry can lag
    // (identity came through the late path while an interview attempt was
    // still failing and wrote UNKNOWN over it -- a Saswell TRV sat at
    // "iv=2 sup=0" forever, and the configure kick below never fired).
    // Heal it here: the kick and the rejoin fast-path both key on MATCHED.
    if (decoded && have &&
        (dev.support_state != static_cast<uint8_t>(SupportState::MATCHED) ||
         dev.interview_state != static_cast<uint8_t>(InterviewState::IDENTITY_READY))) {
        zigbee_pool_with_device(ieee, [](ZapDevice* d, void*) {
            d->interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_READY);
            d->support_state   = static_cast<uint8_t>(SupportState::MATCHED);
        }, nullptr);
        zigbee_pool_mark_dirty();
        ESP_LOGI(TAG, "%016llx decodes with a definition but the pool said iv=%u sup=%u -- marked identified+matched",
                 (unsigned long long)ieee, dev.interview_state, dev.support_state);
        dev.support_state = static_cast<uint8_t>(SupportState::MATCHED);
    }

    // A frame from a device whose configure never completed means it is awake
    // right now -- the only moment a bind / attribute write reaches a sleepy
    // end-device (the cube's binds failed on every timed retry). Re-queue
    // configure while it is listening; the queue skips devices already DONE.
    // Rate-limited per device so a chatty sensor does not queue it per report.
    if (have && dev.support_state == static_cast<uint8_t>(SupportState::MATCHED) &&
        dev.configure_state != static_cast<uint8_t>(ConfigureState::DONE)) {
        static uint64_t s_kick_ieee = 0;
        static int64_t  s_kick_us   = 0;
        const int64_t now_us = esp_timer_get_time();
        if (ieee != s_kick_ieee || now_us - s_kick_us > 30 * 1000000LL) {
            s_kick_ieee = ieee;
            s_kick_us   = now_us;
            ESP_LOGI(TAG, "%016llx is awake and not configured -- running configure now",
                     (unsigned long long)ieee);
            zigbee_configure_enqueue(ieee);
        }
    }

    return false;
}

// The stack sees ZDO (profile 0) and the Groups cluster; everything else is
// consumed here. With endpoint 1 registered (Basic + Groups, for group
// membership) the stack's ZCL engine would otherwise process every device
// report against an endpoint that declares none of those clusters -- on the
// bench that ended in a ZBOSS assert ten seconds after boot. ZHAC decodes
// for its own shadow; the devices get no ZCL replies from the stack, exactly
// as before the endpoint existed.
static bool on_apsde_indication(const ezb_apsde_data_ind_t* ind) {
    if (!ind) return false;
    if (ind->profile_id == 0x0000) return false;          // ZDO: the stack's
    on_apsde_indication_inner(ind);
    return ind->cluster_id != 0x0004;                     // Groups: the stack's too
}

// ── Egress: adapter-encoded ZCL out over APS ─────────────────────────────
// Non-static: esp_zb_interview.cpp sends its Basic-cluster reads through this
// rather than duplicating the apsde plumbing (declared in esp_zb_interview.h).
bool esp_zb_af_send(uint16_t nwk_addr, uint8_t dst_ep, uint16_t cluster_id,
                    const uint8_t* zcl_data, size_t zcl_len) {
    if (!zcl_data || zcl_len == 0) return false;

    ezb_apsde_data_req_t req{};
    req.dst_address.addr_mode   = EZB_ADDR_MODE_SHORT;
    req.dst_address.u.short_addr = nwk_addr;
    req.src_endpoint = 1;
    req.dst_endpoint = dst_ep ? dst_ep : 1;
    req.cluster_id   = cluster_id;
    req.profile_id   = 0x0104;   // Home Automation
    req.radius       = 0;
    req.tx_options   = EZB_APSDE_TX_OPT_ACK_TX;
    req.asdu_length  = static_cast<uint16_t>(zcl_len);
    req.asdu         = const_cast<uint8_t*>(zcl_data);

    // EZB_ERR_NO_MEM (1) means the stack's buffer pool is spent: frames queued
    // for sleeping children hold a buffer each until delivered or expired
    // (~8 s). A sleepy thermostat under interview + configure hit this on
    // every bind, read and DATA_QUERY, so nothing reached it even when awake.
    // From a task, wait for the pool to drain a little instead of failing
    // at once; from the stack's own callback context never block.
    const bool can_wait = xTaskGetCurrentTaskHandle() != zb_lock::stack_task();
    ezb_err_t err = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        {
            zb_lock::Guard g;
            if (!g) return false;
            err = ezb_apsde_data_request(&req);
        }
        if (err != EZB_ERR_NO_MEM || !can_wait) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != 0) {
        ESP_LOGW(TAG, "apsde_data_request to 0x%04x cluster 0x%04x failed (%d)%s",
                 nwk_addr, cluster_id, (int)err,
                 err == EZB_ERR_NO_MEM ? " -- stack buffers full (frames pending for sleeping children)" : "");
        return false;
    }
    return true;
}

// Commands the adapter encodes carry a placeholder TSN (0). The ZNP bridge
// numbers them; this path sent them as they came, so every library command
// from this hub had TSN 0 -- and a Tuya device takes a frame with the TSN of
// the one before for a resend of it. Number them from the shared counter.
// Replies built in this file (genTime, default responses) must keep the
// request's TSN and call esp_zb_af_send directly.
static bool esp_zb_adapter_send(uint16_t nwk_addr, uint8_t dst_ep, uint16_t cluster_id,
                                const uint8_t* zcl_data, size_t zcl_len) {
    uint8_t f[128];
    if (!zcl_data || zcl_len > sizeof(f)) return false;
    const size_t tsn_at = (zcl_data[0] & 0x04) ? 3 : 1;   // manufacturer code sits before it
    if (zcl_len < tsn_at + 2) return false;                // TSN and command id
    std::memcpy(f, zcl_data, zcl_len);
    f[tsn_at] = zcl_seq_next();
    return esp_zb_af_send(nwk_addr, dst_ep, cluster_id, f, zcl_len);
}

// ── Stack lifecycle signals ──────────────────────────────────────────────
//
// This handler is not optional decoration -- it DRIVES commissioning.
// esp_zigbee_start(true) runs BDB *initialisation* only: the stack reads the
// dataset, decides whether a network already exists, and then raises a signal
// asking the application what to do. If nobody answers, the coordinator sits
// there forever with no PAN, and ezb_bdb_open_network() fails with no log of
// its own -- which is exactly how this surfaced: REST returned
// "500 permit_join failed" while the boot log looked perfectly healthy.
static bool on_app_signal(const ezb_app_signal_t* app_signal) {
    if (!app_signal) return false;
    const ezb_app_signal_type_t sig = ezb_app_signal_get_type(app_signal);

    switch (sig) {
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START: {
        // Empty dataset -- form a new PAN. First boot after a flash erase.
        ESP_LOGI(TAG, "no network in dataset -- forming");
        const ezb_err_t e =
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        if (e != 0) {
            ESP_LOGE(TAG, "formation start failed (%d), bdb status %u",
                     (int)e, (unsigned)ezb_bdb_get_commissioning_status());
        }
        return true;
    }

    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        // Dataset already held a network and the stack came back up on it.
        // No formation needed; joined devices keep their addresses.
        s_formed = true;
        ESP_LOGI(TAG, "resumed network: pan 0x%04x channel %u",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel());
        return true;

    case EZB_BDB_SIGNAL_FORMATION:
        s_formed = true;
        spawn_group_join_task();   // first formation of this network
        ESP_LOGI(TAG, "network formed: pan 0x%04x channel %u",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel());
        return true;

    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        // A device joined (or a known one rejoined) and announced itself.
        // Queue it; the interview task owns everything after this point.
        const auto* p = static_cast<const ezb_zdo_signal_device_annce_params_t*>(
            ezb_app_signal_get_params(app_signal));
        if (p) {
            ESP_LOGI(TAG, "device announce: nwk 0x%04x ieee %016llx caps 0x%02x",
                     (unsigned)p->short_addr,
                     (unsigned long long)p->device_addr.u64,
                     (unsigned)p->capability);
            zb_interview_on_announce(p->device_addr.u64, p->short_addr);
        }
        return true;
    }

    case EZB_ZDO_SIGNAL_LEAVE:
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        // Both carry the same params struct. LEAVE is this device being told
        // to leave; LEAVE_INDICATION is a child/neighbour announcing it left.
        // For a coordinator only the latter is expected, but handling both
        // means a stack that reclassifies the event cannot silently strand a
        // device in the pool as permanently present.
        const auto* p = static_cast<const ezb_zdo_signal_leave_indication_params_t*>(
            ezb_app_signal_get_params(app_signal));
        if (p) {
            ESP_LOGI(TAG, "leave: nwk 0x%04x ieee %016llx type %u",
                     (unsigned)p->short_addr,
                     (unsigned long long)p->device_addr.u64,
                     (unsigned)p->leave_type);
            zb_interview_on_leave(p->device_addr.u64);
        }
        return true;
    }

    default:
        // INFO, not DEBUG: this is the only evidence that signals are being
        // delivered at all, and ESP_LOGD is compiled out at the default
        // CONFIG_LOG_MAXIMUM_LEVEL=INFO -- so a DEBUG line here is invisible
        // exactly when it matters.
        ESP_LOGI(TAG, "signal %s (0x%04x)", ezb_app_signal_to_string(sig),
                 (unsigned)sig);
        return false;
    }
}

// ── The Zigbee task ──────────────────────────────────────────────────────
// esp_zigbee_launch_mainloop() does not return; it needs its own task.
static void task_zigbee(void*) {
    const int64_t started_us = esp_timer_get_time();
    // Callbacks run on this task with the SDK lock held by the mainloop;
    // zb_lock::Guard is a no-op here (see esp_zb_lock.h).
    zb_lock::set_stack_task(xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG, "stack mainloop starting");

    // Does not return in normal operation.
    const esp_err_t err = esp_zigbee_launch_mainloop();

    // Getting here means the radio is gone: no joins, no reports, no commands.
    // Nothing in the stack recovers from this by itself, and leaving the
    // firmware "up" with a dead radio is the worst outcome -- REST answers,
    // the SPA renders, and every device silently stops working.
    s_running = false;
    s_formed  = false;
    const int64_t uptime_s = (esp_timer_get_time() - started_us) / 1000000;
    ESP_LOGE(TAG, "stack mainloop EXITED after %llds: %s -- radio is down",
             (long long)uptime_s, esp_err_to_name(err));

    // Reboot to recover, but only if the stack had actually been running for a
    // while. A stack that dies within the first minute would otherwise turn
    // every boot into a reset loop, which is strictly worse than staying up
    // with is_running() == false: that at least leaves REST reachable to say
    // so, and leaves the console usable to diagnose it.
    if (uptime_s >= kMainloopRebootMinUptimeS) {
        ESP_LOGE(TAG, "restarting to recover the radio");
        vTaskDelay(pdMS_TO_TICKS(1000));   // let the log drain
        esp_restart();
    }
    ESP_LOGE(TAG, "died %llds into the run (< %ds) -- NOT restarting, to avoid "
                  "a boot loop. /api/status reports the radio as down.",
             (long long)uptime_s, kMainloopRebootMinUptimeS);
    vTaskDelete(nullptr);   // PSRAM-stack task (zhac_task_create): the stack is not
                            // reclaimed on this dead-radio endpoint, and need not be
}

// ── Boot guard: a radio that kills the chip must not kill the hub ─────────
//
// With no radio firmware on the C6 (a new Guition board ships it with
// ESP-Hosted, not ot_rcp) or a mismatched one, OpenThread's spinel driver
// ends the RCP handshake in DieNow() -> abort(), either inside
// esp_zigbee_init() or a little later from the Zigbee task. Unguarded that is
// a boot loop, and the web UI never stays up long enough to say why.
//
// kGuardArmed sits in no-init RAM from just before radio init until the stack
// has survived kGuardWindowS seconds. It survives a panic or watchdog reset
// but not a power cycle. Finding it armed after such a reset means the last
// boot most likely died starting the radio: skip the radio this boot, keep the
// device pool and UI up, and say so in status. The next reset tries again. An
// unrelated crash inside the window costs one radio-less boot, nothing more.
static __NOINIT_ATTR uint32_t s_boot_guard;
static constexpr uint32_t kGuardArmed   = 0x5A424755;   // "ZBGU"
static constexpr int      kGuardWindowS = 30;
static const char*        s_last_error  = nullptr;

static bool boot_guard_tripped() {
    const esp_reset_reason_t r = esp_reset_reason();
    const bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    const bool tripped = crashed && s_boot_guard == kGuardArmed;
    s_boot_guard = 0;   // no-init RAM is garbage after power-on: always clear
    return tripped;
}

static void boot_guard_arm() {
    static esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args{};
    args.callback = [](void*) { s_boot_guard = 0; };
    args.name     = "zb_boot_guard";
    if (!timer && esp_timer_create(&args, &timer) != ESP_OK) return;  // unguarded, as before
    s_boot_guard = kGuardArmed;
    esp_timer_start_once(timer, static_cast<uint64_t>(kGuardWindowS) * 1000000ULL);
}

const char* esp_zigbee_backend_last_error(void) { return s_last_error; }

// ── DeviceBackend implementation ─────────────────────────────────────────

static bool zb_init() {
    // The dataset partition must be an initialised NVS partition BEFORE
    // esp_zigbee_init: ezb_plat_datasets_init calls nvs_open_from_partition()
    // and abort()s on failure. Note esp-zigbee-lib v2.x uses NVS here, unlike
    // v1.x (and IDF's bundled examples) which used a FAT partition.
    esp_err_t nerr = nvs_flash_init_partition(CONFIG_ZHAC_ZB_STORAGE_PARTITION);
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(CONFIG_ZHAC_ZB_STORAGE_PARTITION));
        nerr = nvs_flash_init_partition(CONFIG_ZHAC_ZB_STORAGE_PARTITION);
    }
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "zigbee storage partition '%s' unavailable: %s -- radio down",
                 CONFIG_ZHAC_ZB_STORAGE_PARTITION, esp_err_to_name(nerr));
        return false;
    }

    // The device pool is normally brought up by zigbee_mgr_init(), which is
    // ZNP-bound and therefore not compiled into this SKU. Nothing else calls
    // these, so without them pool_add() would write through a null pool and
    // every join would be lost. Order matters: init (allocates in PSRAM) ->
    // snapshot cb (so deferred flushes can read live state) -> restore.
    zigbee_pool_init();
    zap_store_set_snapshot_cb(zigbee_pool_snapshot);

    // Install the decode -> device_shadow sink. Normally done by
    // zigbee_mgr_init(), which is ZNP-bound and not compiled here. Must happen
    // BEFORE the radio starts: zhac_adapter_try_decode() drops every key it
    // decodes if no sink is registered, so any frame arriving in the gap is
    // logged as "matched" and then silently lost.
    zhc_shadow_bridge_register();
    const uint16_t restored = zigbee_pool_restore_persisted();
    if (restored) {
        ESP_LOGI(TAG, "restored %u device(s) from NVS", (unsigned)restored);
    }

    // After the pool restore, so a radio-less boot still lists the devices.
    if (boot_guard_tripped()) {
        s_last_error = "radio_crashed";
        ESP_LOGE(TAG, "the previous boot died while starting the radio -- running "
                      "WITHOUT Zigbee this boot; reset to retry. On the P4 board "
                      "this usually means the ESP32-C6 has no ot_rcp firmware.");
        return false;
    }
    boot_guard_arm();

    esp_zigbee_config_t cfg{};
    cfg.device_config.device_type         = EZB_NWK_DEVICE_TYPE_COORDINATOR;
    cfg.device_config.install_code_policy = false;
    cfg.device_config.zczr_config.max_children = CONFIG_ZHAC_ZB_MAX_CHILDREN;
    cfg.platform_config.storage_partition_name = CONFIG_ZHAC_ZB_STORAGE_PARTITION;

#if CONFIG_ZB_RADIO_NATIVE
    cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;
    ESP_LOGI(TAG, "radio: native 802.15.4");
#else
    cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP;
    auto& u = cfg.platform_config.radio_config.radio_uart_config;
    u.port   = static_cast<uart_port_t>(CONFIG_ZHAC_RCP_UART_PORT);
    u.rx_pin = static_cast<gpio_num_t>(CONFIG_ZHAC_RCP_UART_RX_GPIO);
    u.tx_pin = static_cast<gpio_num_t>(CONFIG_ZHAC_RCP_UART_TX_GPIO);
    u.uart_config.baud_rate = 460800;
    u.uart_config.data_bits = UART_DATA_8_BITS;
    u.uart_config.parity    = UART_PARITY_DISABLE;
    u.uart_config.stop_bits = UART_STOP_BITS_1;
    u.uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    u.uart_config.rx_flow_ctrl_thresh = 0;
    u.uart_config.source_clk = UART_SCLK_DEFAULT;
    ESP_LOGI(TAG, "radio: RCP over UART%d (tx=%d rx=%d @460800)",
             CONFIG_ZHAC_RCP_UART_PORT, CONFIG_ZHAC_RCP_UART_TX_GPIO,
             CONFIG_ZHAC_RCP_UART_RX_GPIO);
#endif

    esp_err_t err = esp_zigbee_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_init failed: %s", esp_err_to_name(err));
        s_last_error = "radio_init_failed";
        return false;
    }

    // Hooks BEFORE start, so nothing is missed between start and registration.
    ezb_apsde_data_indication_handler_register(on_apsde_indication);
    const ezb_err_t sig_err = ezb_app_signal_add_handler(on_app_signal);
    if (sig_err != 0) {
        ESP_LOGE(TAG, "app signal handler registration FAILED (%d) -- "
                      "commissioning cannot be driven", (int)sig_err);
    }
    zhac_adapter_register_send(esp_zb_adapter_send);
    zhac_adapter_register_sleepy(esp_zb_is_sleepy);
    register_coordinator_endpoint();
    ezb_bdb_set_primary_channel_set(CONFIG_ZHAC_ZB_CHANNEL_MASK);

    // autostart=true: run the normal BDB startup. v2.x does NOT auto-run
    // initialisation otherwise, so a reboot would silently not rejoin.
    err = esp_zigbee_start(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s", esp_err_to_name(err));
        s_last_error = "radio_init_failed";
        return false;
    }

    if (zhac_task_create(task_zigbee, "TaskZigbee", zhac::stack::kEventBus,
                    nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "TaskZigbee create failed -- stack will not run");
        return false;
    }

    s_running = true;

    // Wait for autostart to produce a PAN, then form explicitly if it did not.
    //
    // esp_zigbee_start(true) is documented to run the whole startup procedure
    // "including formation", and on a fresh coordinator it should. It does not
    // here: the PAN id stays 0xffff and bdb commissioning status stays 0
    // indefinitely (observed over 38 s). No signal is emitted either -- not
    // FORMATION, not DEVICE_FIRST_START, not anything -- because signals are a
    // product of commissioning, and commissioning never began. So there is
    // nothing to wait for and nothing to notice the failure.
    //
    // Kicking NETWORK_FORMATION explicitly fixes it: the PAN appears in ~4 s
    // and the FORMATION signal then arrives normally (pan 0x27bf channel 12 on
    // the bench). The poll-then-kick shape keeps the fast path when a dataset
    // already holds a network, where autostart DOES rejoin on its own.
    for (int i = 0; i < 30 && !zb_network_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!zb_network_ready()) {
        ESP_LOGW(TAG, "no PAN after autostart -- forming explicitly");
        int fe = -1;
        {
            zb_lock::Guard g;
            if (g) fe = static_cast<int>(
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION));
        }
        if (fe != 0) {
            ESP_LOGE(TAG, "formation start failed (%d)", (int)fe);
        } else {
            for (int i = 0; i < 100 && !zb_network_ready(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    } else {
        // Resume path: the dataset already held a PAN and the stack came back
        // up on it. Tempting to do nothing here -- the network exists and
        // frames flow -- but BDB itself never ran, and it refuses to act from
        // that state. ezb_bdb_open_network() returns 3 (EZB_ERR_INV_STATE), so
        // permit_join fails on every boot after the first with a network that
        // looks perfectly healthy.
        //
        // BDB INITIALIZATION is the spec's answer for "device restarted with
        // network parameters already in NVRAM"; it also raises DEVICE_REBOOT.
        int ie = -1;
        {
            zb_lock::Guard g;
            if (g) ie = static_cast<int>(
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION));
        }
        if (ie != 0) {
            ESP_LOGW(TAG, "bdb initialization failed (%d) -- permit_join may "
                          "refuse until the next reboot", (int)ie);
        }
    }

    // Only start join handling once there is a PAN to join. Configure
    // transports must be registered before any interview can finish, since
    // the interview calls zhac_adapter_configure() on success.
    esp_zb_configure_register();
    zb_interview_init();
    // Late-identity enrichment + deferred configure retries. Both are started
    // by zigbee_mgr_init() on the ZNP path; neither is optional for battery
    // devices, which routinely sleep through the interview's Basic read and
    // through the first configure attempt.
    zigbee_identity_init();
    zigbee_configure_init();

    {
        // Main task, mainloop already running: the getters need the SDK lock.
        zb_lock::Guard g;
        if (!g) {
            ESP_LOGW(TAG, "coordinator state unknown at init: SDK lock timeout");
        } else if (zb_network_ready()) {
            s_formed = true;
            ESP_LOGI(TAG, "coordinator up: pan 0x%04x channel %u max_children=%d",
                     (unsigned)ezb_nwk_get_panid(),
                     (unsigned)ezb_nwk_get_current_channel(),
                     CONFIG_ZHAC_ZB_MAX_CHILDREN);
            spawn_group_join_task();
        } else {
            // Honest: the backend is registered and the stack runs, but joins
            // are impossible until a PAN exists. permit_join will say so too.
            ESP_LOGE(TAG, "coordinator has NO network (pan 0x%04x bdb %u) -- "
                          "joins will be refused",
                     (unsigned)ezb_nwk_get_panid(),
                     (unsigned)ezb_bdb_get_commissioning_status());
        }
    }
    return true;
}

static bool zb_is_running() { return s_running; }

// Is there actually a PAN?
//
// s_formed is set from on_app_signal, but that signal only ever fires once
// formation has been STARTED. On this lib build esp_zigbee_start(true) never
// starts it (see zb_init), so for the first ~5 s no signal exists to wait for
// and s_formed alone would leave permit_join refused forever. Asking the stack
// directly is the authoritative check; the signal is the fast path that keeps
// it cheap afterwards.
//
// 0x0000 and 0xffff are both "no network": 0xffff is the broadcast PAN id the
// stack holds before commissioning.
static bool zb_network_ready() {
    if (s_formed) return true;
    zb_lock::Guard g;
    if (!g) return false;
    const ezb_panid_t pan = ezb_nwk_get_panid();
    return pan != 0x0000 && pan != 0xffff;
}

static bool zb_start_discovery(uint8_t duration_s) {
    if (!s_running) {
        ESP_LOGW(TAG, "permit_join refused: stack not running");
        return false;
    }
    zb_lock::Guard g;
    if (!g) return false;
    if (!zb_network_ready()) {
        // Distinguishes "still forming" from a genuine stack error. Without
        // this the REST layer's flat 500 was the only symptom.
        ESP_LOGW(TAG, "permit_join refused: no network (pan 0x%04x ch %u bdb %u)",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel(),
                 (unsigned)ezb_bdb_get_commissioning_status());
        return false;
    }
    const ezb_err_t e = ezb_bdb_open_network(duration_s);
    if (e != 0) {
        ESP_LOGW(TAG, "open_network(%us) failed (%d)", (unsigned)duration_s, (int)e);
        return false;
    }
    ESP_LOGI(TAG, "network open for %us", (unsigned)duration_s);
    return true;
}

static bool zb_stop_discovery() {
    if (!s_running) return false;
    zb_lock::Guard g;
    if (!g || !zb_network_ready()) return false;
    const ezb_err_t e = ezb_bdb_close_network();
    if (e != 0) {
        ESP_LOGW(TAG, "close_network failed (%d)", (int)e);
        return false;
    }
    ESP_LOGI(TAG, "network closed");
    return true;
}

// Tell the device to leave, then forget it locally.
//
// The local removal is unconditional and does not wait for the ZDO response:
// the overwhelmingly common reason to remove a device is that it is already
// dead or out of range, and a UI delete that fails because the device cannot
// be reached is useless. z2m behaves the same way.
// ZDO Mgmt_Leave, same signature the sibling zigbee_mgr declares. device_cmd
// calls it for a soft remove (leave, keep the row); the backend's own
// remove_device below is the hard path (leave, then forget everything).
bool zigbee_leave_req(uint16_t nwk_addr, uint64_t ieee) {
    if (!s_running || !zb_network_ready() || !nwk_addr) return false;
    ezb_zdo_nwk_mgmt_leave_req_t req{};
    req.dst_nwk_addr = nwk_addr;
    req.field.device_addr.u64 = ieee;
    req.field.remove_children = false;   // reassign, do not orphan-purge
    req.field.rejoin          = false;
    zb_lock::Guard g;
    const int e = g ? static_cast<int>(ezb_zdo_nwk_mgmt_leave_req(&req)) : -1;
    if (e != 0) {
        ESP_LOGW(TAG, "leave req for %016llx failed (%d)", (unsigned long long)ieee, (int)e);
        return false;
    }
    return true;
}

static bool zb_backend_remove_device(uint64_t ieee) {
    ZapDevice snap{};
    if (zigbee_pool_snapshot(ieee, &snap)) {
        zigbee_leave_req(snap.nwk_addr, ieee);   // best effort; removed locally either way
    }
    return zb_interview_forget(ieee);
}

static bool zb_backend_interview(uint64_t ieee, uint16_t addr_hint) {
    if (!s_running) return false;
    // A device already in the pool goes through trigger (which reads its
    // current short address). An unknown IEEE with a usable hint is admitted
    // as a fresh join -- that is what the REST "add by address" path wants.
    if (zb_interview_trigger(ieee)) return true;
    if (addr_hint == 0) return false;
    zb_interview_enqueue(ieee, addr_hint);
    return true;
}

static bool zb_get_device_list(ZapDevice* out, uint16_t max, uint16_t* count_out) {
    if (!out || !count_out) return false;
    *count_out = 0;
    zigbee_pool_lock();
    const ZapDevice* all = pool_all();
    const uint16_t n = pool_count();
    uint16_t w = 0;
    for (uint16_t i = 0; i < n && w < max; i++) {
        if (all[i].ieee_addr) out[w++] = all[i];
    }
    zigbee_pool_unlock();
    *count_out = w;
    return true;
}

static bool zb_get_device(uint64_t ieee, ZapDevice* out) {
    return out && zigbee_pool_snapshot(ieee, out);
}

static bool zb_write_attr(uint64_t ieee, uint8_t ep, const char* key, int32_t val) {
    // The adapter owns encoding and calls zb_af_send once it has a frame, but
    // it needs the device's identity to choose a converter and its nwk address
    // to address the frame -- so read the pool first.
    ZapDevice dev{};
    if (!zigbee_pool_snapshot(ieee, &dev)) {
        ESP_LOGW(TAG, "write_attr: %016llx not in pool", (unsigned long long)ieee);
        return false;
    }
    const uint8_t dst_ep = ep ? ep : dev.endpoints[0];
    return zhac_adapter_send_uint(ieee,
                                  dev.model_id[0] ? dev.model_id : nullptr,
                                  dev.manufacturer_name[0] ? dev.manufacturer_name : nullptr,
                                  dev.nwk_addr, dst_ep, key,
                                  static_cast<uint64_t>(val));
}

static DeviceBackend s_backend = {
    .protocol        = PROTO_ZIGBEE,
    .name            = "Zigbee",
    .init            = zb_init,
    // No poll: the stack owns TaskZigbee and its own transport threads.
    .poll            = nullptr,
    .is_running      = zb_is_running,
    .start_discovery = zb_start_discovery,
    .stop_discovery  = zb_stop_discovery,
    // remove / rename still need ZDO round-trips this backend does not issue
    // yet; nullptr stays honest for those.
    .interview       = zb_backend_interview,
    .write_attr      = zb_write_attr,
    .read_attr       = nullptr,
    .get_device_list = zb_get_device_list,
    .get_device      = zb_get_device,
    .remove_device   = zb_backend_remove_device,
    .rename_device   = nullptr,
};

bool esp_zigbee_backend_register(void) {
    if (!device_backend_register(&s_backend)) {
        ESP_LOGE(TAG, "device_backend_register failed");
        return false;
    }
    ESP_LOGI(TAG, "registered as DeviceBackend");
    return true;
}

// ── Radio entry points the control surface calls directly ────────────────
// These replace components/zigbee_mgr/radio_stubs.cpp, which is dropped from
// the build when CONFIG_ZHAC_ESP_ZIGBEE is set. Same signatures as the
// sibling zigbee_mgr declares in zigbee_mgr.h.
bool zigbee_permit_join(uint8_t duration_s) { return zb_start_discovery(duration_s); }

uint64_t zigbee_mgr_coordinator_ieee() {
    ezb_extaddr_t addr{};
    ezb_plat_radio_get_macaddr(addr.u8);
    return addr.u64;
}

// Not yet implemented on this backend -- ZDO bind/unbind and recommission need
// the ZDO request path, which lands with the join-handling increment. Refusing
// loudly beats pretending.
static inline bool not_yet(const char* op) {
    ESP_LOGW(TAG, "%s: not implemented on esp_zigbee_backend yet", op);
    return false;
}
bool zigbee_zdo_bind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                     uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    if (!s_running) return false;
    return esp_zb_zdo_bind(src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep,
                           /*unbind=*/false);
}
bool zigbee_zdo_unbind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                       uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    if (!s_running) return false;
    return esp_zb_zdo_bind(src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep,
                           /*unbind=*/true);
}
bool zigbee_interview_trigger(uint64_t ieee) { return zb_interview_trigger(ieee); }
bool zigbee_force_recommission() { return not_yet("force_recommission"); }

#else   // !CONFIG_ZHAC_ESP_ZIGBEE

// Radio disabled at build time. The refusals for the six radio entry points
// come from components/zigbee_mgr/radio_stubs.cpp instead.
#include "esp_log.h"
bool esp_zigbee_backend_register(void) {
    ESP_LOGW("esp_zb", "built without CONFIG_ZHAC_ESP_ZIGBEE -- no radio backend");
    return false;
}

#endif  // CONFIG_ZHAC_ESP_ZIGBEE
