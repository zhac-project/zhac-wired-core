// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_interview -- Phase 2. Turning a device announcement into a usable
// ZapDevice.
//
// WHAT AN INTERVIEW IS HERE
// -------------------------
// Identical in outcome to the ZNP path in zhac-components/zigbee_mgr, because
// the rest of ZHAC reads the result, not the transport:
//
//   Node_Desc_req      -> logical device type + manufacturer code
//   Active_EP_req      -> endpoint list                     -> TOPOLOGY_READY
//   Simple_Desc_req/ep -> in/out cluster lists, registered with the adapter
//   Basic 0x0000 read  -> manufacturerName + modelIdentifier -> IDENTITY_READY
//   zhac_adapter_has_def()                                   -> support_state
//   persist + EventType::DEVICE_JOIN
//
// Those two identity strings are the whole point: they are what selects one of
// the 6,235 device definitions. Without them a device decodes as nothing.
//
// WHY A TASK AND NOT A CALLBACK CHAIN
// -----------------------------------
// esp-zigbee's ZDO API is asynchronous -- every request takes a completion
// callback. Expressing a 4-stage, per-endpoint, retrying sequence as nested
// callbacks would spread one logical operation across a dozen entry points
// with no obvious place to keep retry state. Instead each request is issued
// and then awaited on a semaphore from a dedicated task, which reads top to
// bottom and mirrors the ZNP implementation's shape. The callbacks do nothing
// but copy their result into a slot and post the semaphore.
//
// Only one interview runs at a time, so a single set of result slots is safe.
// That is also correct behaviour: interviewing two devices at once on a
// single-radio coordinator just makes both slower and more likely to time out.
#include "sdkconfig.h"

#if CONFIG_ZHAC_ESP_ZIGBEE

#include "esp_zb_interview.h"
#include "esp_zigbee_backend.h"   // esp_zigbee_backend_wall_clock_s
#include "esp_zb_lock.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "ezbee/zdo/zdo_dev_srv_disc.h"
#include "ezbee/zdo/zdo_type.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "zhac_task.h"
#include "task_stacks.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "zigbee_interview_utils.h"
#include "zigbee_configure_queue.h"
#include "zigbee_mgr.h"   // zigbee_pool_remove
#include "zcl_seq.h"
#include "zigbee_pool.h"

#include <cstring>

static const char* TAG = "esp_zb_iv";

namespace {

struct JoinReq {
    uint64_t ieee;
    uint16_t nwk;
};

QueueHandle_t     s_join_q   = nullptr;
SemaphoreHandle_t s_step_sem = nullptr;

// ── ZDO result slots ─────────────────────────────────────────────────────
// Written by the stack's callback, read by the interview task after taking
// s_step_sem. Single-interview-at-a-time makes one set sufficient.
bool     s_step_ok = false;
uint8_t  s_ep_list[8];
uint8_t  s_ep_count = 0;
uint16_t s_in_clusters[ZAP_CLUSTERS_PER_EP];
uint16_t s_out_clusters[ZAP_CLUSTERS_PER_EP];
uint8_t  s_n_in = 0, s_n_out = 0;
uint16_t s_profile_id = 0, s_device_id = 0;
uint8_t  s_logical_type = 0;
uint16_t s_node_mfg_code = 0;

// ── Basic-read sniffing ──────────────────────────────────────────────────
// The Basic cluster read goes out as a raw ZCL frame over APS, so its reply
// arrives through the normal APS indication path rather than a ZDO callback.
// s_basic_nwk is the "armed" flag: non-zero means the interview task is
// waiting for a Basic response from that short address.
volatile uint16_t s_basic_nwk = 0;
SemaphoreHandle_t s_basic_sem = nullptr;
uint8_t           s_basic_buf[128];
uint8_t           s_basic_len = 0;

// Device being interviewed right now, 0 = idle. Read by zb_interview_enqueue
// to drop repeat announces: a joining device often announces several times
// while the interview is still running, and each of those would otherwise
// occupy a queue slot and trigger a full redundant re-interview afterwards.
volatile uint64_t s_active_ieee = 0;

constexpr uint32_t kZdoTimeoutMs   = 5000;
constexpr uint32_t kBasicTimeoutMs = 5000;

// Sleepy end-devices (Aqara buttons, contact sensors) only wake briefly.
// Matches the ZNP path's budget: 10 attempts, 30 s apart, covers typical
// 2-5 minute Xiaomi wake cycles. Mains devices finish on attempt 1.
constexpr int      kMaxAttempts    = 10;
constexpr uint32_t kRetryDelayMs   = 30000;

void step_done(bool ok) {
    s_step_ok = ok;
    xSemaphoreGive(s_step_sem);
}

bool step_wait(uint32_t timeout_ms) {
    if (xSemaphoreTake(s_step_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    return s_step_ok;
}

// ── ZDO callbacks: copy into the slot, post, return ───────────────────────

void on_node_desc(const ezb_zdo_node_desc_req_result_t* r, void*) {
    if (!r || r->error != 0 || !r->rsp || r->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        step_done(false);
        return;
    }
    // Logical type is the low 3 bits of node_flags: 0 = coordinator,
    // 1 = router, 2 = end device. Same field the ZNP path masked with 0x07.
    s_logical_type  = static_cast<uint8_t>(r->rsp->node_desc.node_flags & 0x07);
    s_node_mfg_code = r->rsp->node_desc.manufacturer_code;
    step_done(true);
}

void on_active_ep(const ezb_zdo_active_ep_req_result_t* r, void*) {
    if (!r || r->error != 0 || !r->rsp || r->rsp->status != EZB_ZDP_STATUS_SUCCESS ||
        !r->rsp->active_ep_list) {
        step_done(false);
        return;
    }
    s_ep_count = r->rsp->active_ep_count;
    if (s_ep_count > sizeof(s_ep_list)) s_ep_count = sizeof(s_ep_list);
    std::memcpy(s_ep_list, r->rsp->active_ep_list, s_ep_count);
    step_done(s_ep_count > 0);
}

void on_simple_desc(const ezb_zdo_simple_desc_req_result_t* r, void*) {
    if (!r || r->error != 0 || !r->rsp || r->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        step_done(false);
        return;
    }
    const ezb_af_simple_desc_t& d = r->rsp->desc;
    s_profile_id = d.app_profile_id;
    s_device_id  = d.app_device_id;
    s_n_in  = d.app_input_cluster_count;
    s_n_out = d.app_output_cluster_count;
    if (s_n_in  > ZAP_CLUSTERS_PER_EP) s_n_in  = ZAP_CLUSTERS_PER_EP;
    if (s_n_out > ZAP_CLUSTERS_PER_EP) s_n_out = ZAP_CLUSTERS_PER_EP;

    // One flat array: inputs first, outputs from app_input_cluster_count.
    // Note the truncation above applies to what we STORE; the offset into the
    // source list must still use the device's real input count.
    if (d.app_cluster_list) {
        for (uint8_t i = 0; i < s_n_in; i++) s_in_clusters[i] = d.app_cluster_list[i];
        for (uint8_t i = 0; i < s_n_out; i++) {
            s_out_clusters[i] = d.app_cluster_list[d.app_input_cluster_count + i];
        }
    } else {
        s_n_in = s_n_out = 0;
    }
    step_done(true);
}

// ── ZDO request wrappers ──────────────────────────────────────────────────

bool req_node_desc(uint16_t nwk) {
    xSemaphoreTake(s_step_sem, 0);   // drain a stale post
    ezb_zdo_node_desc_req_t req{};
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.cb = on_node_desc;
    {
        // Lock only around the request; the result is posted by a callback on
        // the stack task, which needs the lock back before step_wait can end.
        zb_lock::Guard g;
        if (!g || ezb_zdo_node_desc_req(&req) != 0) return false;
    }
    return step_wait(kZdoTimeoutMs);
}

bool req_active_ep(uint16_t nwk) {
    xSemaphoreTake(s_step_sem, 0);
    ezb_zdo_active_ep_req_t req{};
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.cb = on_active_ep;
    {
        // Lock only around the request; the result is posted by a callback on
        // the stack task, which needs the lock back before step_wait can end.
        zb_lock::Guard g;
        if (!g || ezb_zdo_active_ep_req(&req) != 0) return false;
    }
    return step_wait(kZdoTimeoutMs);
}

bool req_simple_desc(uint16_t nwk, uint8_t ep) {
    xSemaphoreTake(s_step_sem, 0);
    ezb_zdo_simple_desc_req_t req{};
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.field.endpoint = ep;
    req.cb = on_simple_desc;
    {
        // Lock only around the request; the result is posted by a callback on
        // the stack task, which needs the lock back before step_wait can end.
        zb_lock::Guard g;
        if (!g || ezb_zdo_simple_desc_req(&req) != 0) return false;
    }
    return step_wait(kZdoTimeoutMs);
}

// ── Basic-cluster identity read ───────────────────────────────────────────

// Profile-wide Read Attributes on cluster 0x0000.
//   [0] frame control 0x00 -- profile-wide, client->server, no mfg code
//   [1] transaction sequence number
//   [2] command 0x00 = Read Attributes
//   [3..] attribute ids, little endian
bool read_basic_once(uint16_t nwk, uint8_t ep, uint8_t tsn) {
    static const uint8_t kAttrs[] = {
        0x01, 0x00,   // 0x0001 manufacturerCode (parsed by the shared helper)
        0x04, 0x00,   // 0x0004 manufacturerName
        0x05, 0x00,   // 0x0005 modelIdentifier
        0x07, 0x00,   // 0x0007 powerSource -- ignored by the helper, harmless
    };
    uint8_t frame[3 + sizeof(kAttrs)];
    frame[0] = 0x00;
    frame[1] = tsn;
    frame[2] = 0x00;
    std::memcpy(frame + 3, kAttrs, sizeof(kAttrs));

    s_basic_len = 0;
    xSemaphoreTake(s_basic_sem, 0);
    s_basic_nwk = nwk;              // arm the sniffer before sending

    const bool sent = esp_zb_af_send(nwk, ep, 0x0000, frame, sizeof(frame));
    if (!sent) {
        s_basic_nwk = 0;
        return false;
    }
    const bool got = xSemaphoreTake(s_basic_sem, pdMS_TO_TICKS(kBasicTimeoutMs)) == pdTRUE;
    s_basic_nwk = 0;
    return got && s_basic_len > 0;
}

// Pull Basic attr 0x0007 (powerSource, ENUM8) out of a Read Attributes
// Response. zigbee_parse_basic_identity extracts model / manufacturer /
// manufacturer-code but not this one, so it stayed 0 ("unknown") on every
// device and the UI showed "Power Source: —" for a battery sensor that had
// just told us it runs on a battery.
//
// Record layout after the 3-byte header: attr_id(2 LE) | status(1) |
// [type(1) | value(N)] -- type+value present only when status == SUCCESS.
// Walk it rather than assuming attribute order; devices reorder freely.
uint8_t parse_power_source(const uint8_t* d, uint8_t len) {
    if (!d || len < 3) return 0;
    size_t p = ((d[0] & 0x04) ? 5u : 3u);   // skip header (+2 if manuf-specific)
    while (p + 3 <= len) {
        const uint16_t attr = static_cast<uint16_t>(d[p]) |
                              static_cast<uint16_t>(d[p + 1] << 8);
        const uint8_t status = d[p + 2];
        p += 3;
        if (status != 0x00) continue;       // failed read: no type/value follows
        if (p >= len) break;
        const uint8_t type = d[p++];
        uint8_t vlen = 0;
        switch (type) {
            case 0x30: case 0x20: case 0x18: case 0x10: vlen = 1; break;
            case 0x31: case 0x21: case 0x29: case 0x19: vlen = 2; break;
            case 0x23: case 0x2B: vlen = 4; break;
            case 0x42:                       // char string: length-prefixed
                if (p >= len) return 0;
                vlen = static_cast<uint8_t>(1 + d[p]);
                break;
            default: return 0;               // unknown width -> cannot walk on
        }
        if (p + vlen > len) break;
        if (attr == 0x0007 && vlen >= 1) return d[p];
        p += vlen;
    }
    return 0;
}

// Probe endpoints that advertise Basic first (shared helper), then the rest.
bool read_basic(ZapDevice* work, uint16_t nwk) {
    uint8_t order[8];
    const uint8_t n = zigbee_interview_build_basic_probe_order(*work, order, sizeof(order));
    for (uint8_t i = 0; i < n; i++) {
        if (!read_basic_once(nwk, order[i], zcl_seq_next())) continue;

        // manufacturer_code from the Basic read wins over the node descriptor's
        // only when the read actually carried it; seed with what we have.
        uint16_t mfg_code = work->manufacturer_code;
        const bool any = zigbee_parse_basic_identity(
            s_basic_buf, s_basic_len,
            work->model_id, sizeof(work->model_id),
            work->manufacturer_name, sizeof(work->manufacturer_name),
            &mfg_code);
        if (const uint8_t ps = parse_power_source(s_basic_buf, s_basic_len)) {
            work->power_source = ps;
        }
        if (any) {
            work->manufacturer_code = mfg_code;
            if (work->model_id[0] || work->manufacturer_name[0]) return true;
        }
    }
    return false;
}

// ── The interview ─────────────────────────────────────────────────────────

// Copy the finished record back into the live pool slot under the lock.
struct CommitCtx {
    const ZapDevice* src;
    bool             nwk_changed;
};

void commit_fn(ZapDevice* live, void* ctx) {
    auto* c = static_cast<CommitCtx*>(ctx);
    const uint16_t old_nwk = live->nwk_addr;
    // friendly_name is user-owned -- never clobber it from an interview.
    char keep[sizeof(live->friendly_name)];
    std::memcpy(keep, live->friendly_name, sizeof(keep));
    *live = *c->src;
    std::memcpy(live->friendly_name, keep, sizeof(keep));
    c->nwk_changed = (old_nwk != live->nwk_addr);
}

bool do_interview(uint64_t ieee, uint16_t nwk) {
    ZapDevice work{};
    if (!zigbee_pool_snapshot(ieee, &work)) {
        ESP_LOGW(TAG, "%016llx vanished from pool before interview",
                 (unsigned long long)ieee);
        return false;
    }
    work.nwk_addr  = nwk;
    if (const uint32_t t = esp_zigbee_backend_wall_clock_s()) work.last_seen = t;

    // 1. Node descriptor -- logical type + manufacturer code. Not fatal:
    //    plenty of devices answer Active_EP but stall on Node_Desc.
    if (req_node_desc(nwk)) {
        work.device_type       = s_logical_type;
        work.manufacturer_code = s_node_mfg_code;
    } else {
        ESP_LOGD(TAG, "%016llx node_desc failed (non-fatal)",
                 (unsigned long long)ieee);
    }

    // 2. Active endpoints. Fatal -- without endpoints there is nothing to read.
    if (!req_active_ep(nwk)) {
        work.interview_state = static_cast<uint8_t>(InterviewState::FAILED);
        CommitCtx c{&work, false};
        zigbee_pool_with_device(ieee, commit_fn, &c);
        return false;
    }
    work.endpoint_count = s_ep_count;
    std::memcpy(work.endpoints, s_ep_list, s_ep_count);
    std::memset(work.clusters, 0, sizeof(work.clusters));
    std::memset(work.clusters_out, 0, sizeof(work.clusters_out));

    // 3. Simple descriptor per endpoint -> cluster lists.
    for (uint8_t i = 0; i < s_ep_count && i < 8; i++) {
        const uint8_t ep = work.endpoints[i];
        if (!req_simple_desc(nwk, ep)) {
            ESP_LOGW(TAG, "%016llx ep %u simple_desc failed",
                     (unsigned long long)ieee, ep);
            continue;
        }
        for (uint8_t c = 0; c < s_n_in;  c++) work.clusters[i][c]     = s_in_clusters[c];
        for (uint8_t c = 0; c < s_n_out; c++) work.clusters_out[i][c] = s_out_clusters[c];

        // Feeds the adapter's cluster-aware fallback, so standard ZCL devices
        // with no library definition still expose something usable.
        zhac_adapter_register_endpoint(ieee, ep, s_profile_id, s_device_id,
                                       s_in_clusters, s_n_in,
                                       s_out_clusters, s_n_out);
    }
    work.interview_state = static_cast<uint8_t>(InterviewState::TOPOLOGY_READY);

    // 4. Identity. This is the step that decides whether the device is usable,
    //    and the step sleepy devices fail -- hence the retry loop around the
    //    whole interview rather than around individual requests.
    const bool have_identity = read_basic(&work, nwk);
    work.interview_state = static_cast<uint8_t>(
        have_identity ? InterviewState::IDENTITY_READY : InterviewState::IDENTITY_PENDING);

    // 5. Definition match.
    if (have_identity) {
        const bool supported = zhac_adapter_has_def(
            ieee,
            work.model_id[0] ? work.model_id : nullptr,
            work.manufacturer_name[0] ? work.manufacturer_name : nullptr);
        work.support_state = static_cast<uint8_t>(
            supported ? SupportState::MATCHED : SupportState::UNMATCHED);

        // Definitions may pin power source for devices that misreport it
        // (z2m's forcePowerSource).
        const uint8_t override_ps = zhac_adapter_power_source_override(
            work.model_id[0] ? work.model_id : nullptr,
            work.manufacturer_name[0] ? work.manufacturer_name : nullptr);
        if (override_ps) work.power_source = override_ps;
    } else {
        work.support_state = static_cast<uint8_t>(SupportState::UNKNOWN);
    }

    CommitCtx c{&work, false};
    if (!zigbee_pool_with_device(ieee, commit_fn, &c)) return false;
    if (c.nwk_changed) zigbee_pool_mark_dirty();   // nwk index must be rebuilt

    ESP_LOGI(TAG, "interview %016llx nwk 0x%04x: '%s' / '%s' eps=%u iv=%u sup=%u",
             (unsigned long long)ieee, nwk,
             work.manufacturer_name, work.model_id,
             work.endpoint_count, work.interview_state, work.support_state);

    // Persist outside the visitor -- mark_dirty can write flash on overflow,
    // and the pool mutex must not be held across that.
    zap_store_mark_dirty(&work, ZAP_PERSIST_HIGH);

    // Configure: bindings + reporting + config_steps. Without this the device
    // is identified but inert -- nothing has told it to send its reports here.
    // Only meaningful once identity is known, since the definition (and hence
    // its bindings[]/reports[]) is selected by model+manufacturer.
    //
    // Enqueued rather than run inline. A single attempt at the end of the
    // interview is exactly the wrong moment for a battery device: it has often
    // gone back to sleep by then, every bind silently fails, and nothing ever
    // tries again -- the device sits MATCHED with no bindings and no reports.
    // The queue owns the retry schedule (1/5/30/120/600 s) and the
    // ConfigureState/attempts bookkeeping that used to be duplicated here.
    if (have_identity) {
        zigbee_configure_enqueue(ieee);
    }
    return have_identity;
}

// Insert the device if new, or refresh its short address if it rejoined.
// Returns false only if the pool is full.
bool pool_upsert(uint64_t ieee, uint16_t nwk) {
    zigbee_pool_lock();
    ZapDevice* d = pool_find_by_ieee(ieee);
    if (!d) {
        d = pool_add();
        if (!d) {
            zigbee_pool_unlock();
            ESP_LOGE(TAG, "pool full -- cannot admit %016llx",
                     (unsigned long long)ieee);
            return false;
        }
        std::memset(d, 0, sizeof(*d));
        d->ieee_addr       = ieee;
        d->protocol        = PROTO_ZIGBEE;
        d->interview_state = static_cast<uint8_t>(InterviewState::NONE);
        d->support_state   = static_cast<uint8_t>(SupportState::UNKNOWN);
        d->configure_state = static_cast<uint8_t>(ConfigureState::PENDING);
    }
    const bool nwk_changed = (d->nwk_addr != nwk);
    d->nwk_addr  = nwk;
    if (const uint32_t t = esp_zigbee_backend_wall_clock_s()) d->last_seen = t;
    zigbee_pool_unlock();
    if (nwk_changed) zigbee_pool_mark_dirty();
    return true;
}

void task_interview(void*) {
    JoinReq req{};
    for (;;) {
        if (xQueueReceive(s_join_q, &req, portMAX_DELAY) != pdTRUE) continue;
        if (req.ieee == 0) continue;

        if (!pool_upsert(req.ieee, req.nwk)) continue;
        s_active_ieee = req.ieee;

        bool ok = false;
        for (int attempt = 1; attempt <= kMaxAttempts && !ok; attempt++) {
            ok = do_interview(req.ieee, req.nwk);
            if (ok) break;

            // Re-read the short address: a sleepy device may have rejoined
            // under a new one between attempts, and retrying the old address
            // would fail forever.
            ZapDevice snap{};
            if (zigbee_pool_snapshot(req.ieee, &snap)) {
                if (snap.nwk_addr) req.nwk = snap.nwk_addr;
                if (snap.interview_state ==
                    static_cast<uint8_t>(InterviewState::FAILED)) {
                    // Active_EP failed outright; still worth another window.
                }
            }
            if (attempt < kMaxAttempts) {
                ESP_LOGI(TAG, "%016llx identity incomplete -- retry %d/%d in %us",
                         (unsigned long long)req.ieee, attempt + 1, kMaxAttempts,
                         (unsigned)(kRetryDelayMs / 1000));
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            }
        }

        if (!ok) {
            ESP_LOGW(TAG, "%016llx interview incomplete after %d attempts -- "
                          "device is in the pool but has no identity",
                     (unsigned long long)req.ieee, kMaxAttempts);
        }

        // Announce either way. An unidentified device must still appear in the
        // UI: that is how an operator learns it needs attention, and it is what
        // /api/device/reinterview acts on.
        Event ev{};
        ev.type = EventType::DEVICE_JOIN;
        const uint64_t ieee = req.ieee;
        std::memcpy(ev.data, &ieee, sizeof(ieee));
        event_bus_publish(ev);

        s_active_ieee = 0;
    }
}

}  // namespace

// ── Public seam ───────────────────────────────────────────────────────────

void zb_interview_init() {
    if (s_join_q) return;
    s_step_sem  = xSemaphoreCreateBinary();
    s_basic_sem = xSemaphoreCreateBinary();
    s_join_q    = zhac_queue_create(16, sizeof(JoinReq));
    if (!s_step_sem || !s_basic_sem || !s_join_q) {
        ESP_LOGE(TAG, "alloc failed -- joins will not be interviewed");
        return;
    }
    if (zhac_task_create(task_interview, "TaskZbIv", zhac::stack::kZbInterview,
                    nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "TaskZbIv create failed -- joins will not be interviewed");
        return;
    }
    ESP_LOGI(TAG, "join handling ready (queue=16, %d attempts x %us)",
             kMaxAttempts, (unsigned)(kRetryDelayMs / 1000));
}

void zb_interview_enqueue(uint64_t ieee, uint16_t nwk) {
    if (!s_join_q || ieee == 0) return;
    if (s_active_ieee == ieee) {
        // Already being interviewed. The running attempt re-reads the short
        // address from the pool between retries, so a rejoin under a new
        // address is picked up without re-queueing.
        ESP_LOGD(TAG, "announce for %016llx ignored -- interview in progress",
                 (unsigned long long)ieee);
        return;
    }
    const JoinReq r{ieee, nwk};
    if (xQueueSend(s_join_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "join queue full -- dropping announce for %016llx",
                 (unsigned long long)ieee);
    }
}

// ── Lifecycle: announce / leave / forget ──────────────────────────────────

namespace {

// Clear the soft-removed flag and refresh the address, in one locked pass.
struct RejoinCtx {
    uint16_t nwk;
    uint32_t now;
    bool     nwk_changed;
    bool     was_removed;
    ZapDevice snap;
};

void rejoin_fn(ZapDevice* d, void* ctx) {
    auto* c = static_cast<RejoinCtx*>(ctx);
    c->nwk_changed = (d->nwk_addr != c->nwk);
    c->was_removed = zap_dev_is_removed(d);
    d->flags      &= static_cast<uint8_t>(~ZAP_DEV_REMOVED);
    d->nwk_addr    = c->nwk;
    if (c->now) d->last_seen = c->now;
    c->snap        = *d;
}

void leave_fn(ZapDevice* d, void* ctx) {
    zap_dev_mark_removed(d);
    *static_cast<ZapDevice*>(ctx) = *d;
}

}  // namespace

void zb_interview_on_announce(uint64_t ieee, uint16_t nwk) {
    if (ieee == 0) return;

    RejoinCtx c{};
    c.nwk = nwk;
    c.now = esp_zigbee_backend_wall_clock_s();

    if (zigbee_pool_with_device(ieee, rejoin_fn, &c)) {
        // Known device. A full re-interview is only warranted when we never
        // got its identity -- routers announce on every power cycle, and
        // re-running four ZDO round-trips per device across a mains outage
        // would serialise the whole network behind the radio for minutes.
        if (c.nwk_changed) zigbee_pool_mark_dirty();   // nwk index is stale

        if (c.snap.interview_state ==
            static_cast<uint8_t>(InterviewState::IDENTITY_READY)) {
            ESP_LOGI(TAG, "rejoin %016llx nwk 0x%04x%s (identity known -- "
                          "no re-interview)",
                     (unsigned long long)ieee, nwk,
                     c.was_removed ? ", un-removed" : "");
            zap_store_mark_dirty(&c.snap, ZAP_PERSIST_LOW);
            Event ev{};
            ev.type = EventType::DEVICE_JOIN;
            std::memcpy(ev.data, &ieee, sizeof(ieee));
            event_bus_publish(ev);
            return;
        }
        // Known but never identified -- a rejoin is exactly the wake window
        // the retry loop was waiting for.
        zap_store_mark_dirty(&c.snap, ZAP_PERSIST_LOW);
    }

    zb_interview_enqueue(ieee, nwk);
}

void zb_interview_on_leave(uint64_t ieee) {
    if (ieee == 0) return;
    // Soft-remove. The record stays in pool + NVS so friendly name, interview
    // state and shadow cache survive until the device rejoins (flag cleared in
    // zb_interview_on_announce) or the user hard-deletes it. Same contract as
    // the ZNP path's ZDO_LEAVE_IND handler.
    ZapDevice snap{};
    if (zigbee_pool_with_device(ieee, leave_fn, &snap)) {
        // Persist OUTSIDE the visitor: mark_dirty can write flash synchronously
        // when the dirty table is full, and that must not run under the pool
        // mutex.
        zap_store_mark_dirty(&snap, ZAP_PERSIST_LOW);
        ESP_LOGI(TAG, "device left: %016llx (soft-remove)",
                 (unsigned long long)ieee);
    } else {
        ESP_LOGI(TAG, "leave from unknown device %016llx -- ignored",
                 (unsigned long long)ieee);
    }
    Event ev{};
    ev.type = EventType::DEVICE_LEAVE;
    std::memcpy(ev.data, &ieee, sizeof(ieee));
    event_bus_publish(ev);
}

bool zb_interview_forget(uint64_t ieee) {
    if (ieee == 0) return false;
    const bool gone = zigbee_pool_remove(ieee);   // also invalidates the def cache
    zhac_adapter_fallback_clear(ieee);            // and the cluster fallback data
    zap_store_delete_device(ieee);
    if (gone) {
        ESP_LOGI(TAG, "device forgotten: %016llx", (unsigned long long)ieee);
        Event ev{};
        ev.type = EventType::DEVICE_LEAVE;
        std::memcpy(ev.data, &ieee, sizeof(ieee));
        event_bus_publish(ev);
    }
    return gone;
}

bool zb_interview_trigger(uint64_t ieee) {
    ZapDevice snap{};
    if (!zigbee_pool_snapshot(ieee, &snap)) {
        ESP_LOGW(TAG, "reinterview: %016llx not in pool",
                 (unsigned long long)ieee);
        return false;
    }
    if (!s_join_q) return false;
    const JoinReq r{ieee, snap.nwk_addr};
    return xQueueSend(s_join_q, &r, 0) == pdTRUE;
}

bool zb_interview_feed_zcl(uint16_t nwk, uint16_t cluster_id, uint8_t /*src_ep*/,
                           const uint8_t* zcl, uint8_t zcl_len) {
    if (cluster_id != 0x0000 || !zcl || zcl_len == 0) return false;
    if (s_basic_nwk == 0 || s_basic_nwk != nwk) return false;

    // Accept Read Attributes Response (0x01) and Report Attributes (0x0A);
    // both carry the identity attributes and the shared parser handles either.
    // Byte 0 is frame control, byte 1 the TSN, byte 2 the command -- but only
    // for a non-manufacturer-specific frame (frame control bit 2 clear), which
    // adds a 2-byte code before the TSN.
    const bool mfg_specific = (zcl[0] & 0x04) != 0;
    const uint8_t cmd_idx   = mfg_specific ? 4 : 2;
    if (zcl_len <= cmd_idx) return false;
    const uint8_t cmd = zcl[cmd_idx];
    if (cmd != 0x01 && cmd != 0x0A) return false;

    s_basic_len = (zcl_len > sizeof(s_basic_buf))
                      ? static_cast<uint8_t>(sizeof(s_basic_buf))
                      : zcl_len;
    std::memcpy(s_basic_buf, zcl, s_basic_len);
    xSemaphoreGive(s_basic_sem);
    return true;
}

#endif  // CONFIG_ZHAC_ESP_ZIGBEE
