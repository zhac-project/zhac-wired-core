// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_backend.cpp's delivery path, cut out at build time between "tx-delivery begin" and "tx-delivery end"
// (see CMakeLists.txt), against stubs of the stack, the pool and the adapter: what the hub does with a frame the
// stack could not deliver (research 2026-11-18 B1). A device that keeps its receiver on is looked up again (ZDO
// NWK_addr_req) and the frame resent once, to its new address when it moved; nobody answering marks it offline.
// A collection's On / Off / Toggle goes out as one groupcast within the same broadcast budget, and the copy of
// it the stack hands back to the hub's own endpoint is recognised as the hub's (B5).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "../../esp_zb_bcast_budget.h"
#include "../../esp_zb_tx_track.h"

// ── ESP-IDF and FreeRTOS, as far as the slice uses them ─────────────────────
#define EXT_RAM_BSS_ATTR
struct portMUX_TYPE { int unused; };
#define portMUX_INITIALIZER_UNLOCKED {0}
#define taskENTER_CRITICAL(m) (void)(m)
#define taskEXIT_CRITICAL(m) (void)(m)
static std::int64_t g_now_us = 0;
static std::int64_t esp_timer_get_time() { return g_now_us; }
using TaskHandle_t = void*;
static int g_stack_task;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return &g_stack_task; }   // confirms run on the stack task
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(std::uint32_t) {}
static const char* TAG = "esp_zigbee";
__attribute__((format(printf, 2, 3))) static void log_line(const char*, const char*, ...) {}
#define ESP_LOGW(tag, ...) log_line(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) log_line(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) log_line(tag, __VA_ARGS__)
namespace zb_lock {
static bool g_ok = true;   // false: the stack's lock could not be taken
static TaskHandle_t stack_task() { return &g_stack_task; }
struct Guard {
    explicit operator bool() const { return g_ok; }
};
}  // namespace zb_lock

// ── esp-zigbee-lib, as far as the slice uses it (same names as ezbee/*.h) ───
using ezb_err_t = int;
constexpr ezb_err_t EZB_ERR_NO_MEM = 1;
enum ezb_addr_mode_t { EZB_ADDR_MODE_NONE, EZB_ADDR_MODE_GROUP, EZB_ADDR_MODE_SHORT, EZB_ADDR_MODE_EXT };
struct ezb_extaddr_t { std::uint64_t u64; };
struct ezb_grpaddr_t { std::uint16_t group, bcast; };
union ezb_addr_t { std::uint16_t short_addr; ezb_grpaddr_t group_addr; ezb_extaddr_t extended_addr; };
struct ezb_address_t { ezb_addr_mode_t addr_mode; ezb_addr_t u; };
enum { EZB_APSDE_TX_OPT_ACK_TX = 0x04 };
struct ezb_apsde_data_req_t {
    ezb_address_t dst_address;
    std::uint8_t  src_endpoint, dst_endpoint;
    std::uint16_t cluster_id, profile_id;
    std::uint8_t  radius, tx_options;
    std::uint16_t asdu_length;
    std::uint8_t* asdu;
};
struct ezb_apsde_data_confirm_t {
    ezb_address_t dst_address;
    std::uint8_t  src_endpoint, dst_endpoint;
    std::uint16_t cluster_id, profile_id;
    std::uint8_t  status;
    std::uint16_t asdu_length;
    std::uint8_t* asdu;
};
struct ezb_zdp_address_rsp_field_t {
    std::uint8_t  status;
    ezb_extaddr_t ieee_addr_remote_dev;
    std::uint16_t nwk_addr_remote_dev;
};
struct ezb_zdo_nwk_addr_req_result_t { ezb_err_t error; ezb_zdp_address_rsp_field_t* rsp; };
using ezb_zdo_nwk_addr_req_callback_t = void (*)(const ezb_zdo_nwk_addr_req_result_t*, void*);
struct ezb_zdp_nwk_addr_req_field_t { ezb_extaddr_t ieee_addr_of_interest; std::uint8_t request_type, start_index; };
struct ezb_zdo_nwk_addr_req_t {
    std::uint16_t                   dst_nwk_addr;
    ezb_zdp_nwk_addr_req_field_t    field;
    ezb_zdo_nwk_addr_req_callback_t cb;
    void*                           user_ctx;
};

// The radio: what was sent, and what the stack says to each request.
struct Sent {
    std::uint16_t              nwk;   // or the group, for a groupcast
    std::uint8_t               ep;
    std::uint16_t              cluster;
    std::vector<std::uint8_t> bytes;
    ezb_addr_mode_t            mode;
    std::uint16_t              bcast;   // a groupcast's broadcast address
    std::uint8_t               src_ep, tx_options;
    std::uint16_t              profile;
};
static std::vector<Sent> g_sent;
static ezb_err_t g_aps_err = 0;
static ezb_err_t ezb_apsde_data_request(const ezb_apsde_data_req_t* r) {
    if (g_aps_err) return g_aps_err;
    g_sent.push_back({r->dst_address.u.short_addr, r->dst_endpoint, r->cluster_id,
                      std::vector<std::uint8_t>(r->asdu, r->asdu + r->asdu_length), r->dst_address.addr_mode,
                      r->dst_address.u.group_addr.bcast, r->src_endpoint, r->tx_options, r->profile_id});
    return 0;
}
static std::uint8_t g_tsn = 0x40;
static std::uint8_t zcl_seq_next() { return ++g_tsn; }
constexpr std::uint64_t kCoord = 0x00124B0000000C00ull;
static std::uint64_t g_coord = kCoord;   // 0: the hub's IEEE not known yet
static std::uint64_t zigbee_mgr_coordinator_ieee() { return g_coord; }
static std::vector<ezb_zdo_nwk_addr_req_t> g_lookups;   // every NWK_addr_req asked for, refused ones too
static ezb_err_t g_zdo_err = 0;
static ezb_err_t ezb_zdo_nwk_addr_req(const ezb_zdo_nwk_addr_req_t* r) {
    g_lookups.push_back(*r);
    return g_zdo_err;
}

// The device pool (who the hub addressed), the stack's own address map, and the adapter.
struct ZapDevice { std::uint64_t ieee_addr; std::uint16_t nwk_addr; };
struct Dev { std::uint64_t ieee; std::uint16_t nwk; bool sleepy; };
static std::vector<Dev> g_pool;
static bool zigbee_pool_snapshot_by_nwk(std::uint16_t nwk, ZapDevice* d) {
    for (const Dev& x : g_pool) {
        if (x.nwk == nwk) { d->ieee_addr = x.ieee; d->nwk_addr = x.nwk; return true; }
    }
    return false;
}
static std::vector<std::pair<std::uint16_t, std::uint64_t>> g_map;
static ezb_err_t ezb_address_extended_by_short(std::uint16_t nwk, ezb_extaddr_t* e) {
    for (const auto& m : g_map) {
        if (m.first == nwk) { e->u64 = m.second; return 0; }
    }
    return 1;
}
static bool esp_zb_is_sleepy(std::uint64_t ieee) {
    for (const Dev& x : g_pool) {
        if (x.ieee == ieee) return x.sleepy;
    }
    return false;
}
static std::vector<std::pair<std::uint64_t, std::uint16_t>> g_moved;
static void pool_set_nwk(std::uint64_t ieee, std::uint16_t nwk) {
    g_moved.push_back({ieee, nwk});
    for (Dev& x : g_pool) {
        if (x.ieee == ieee) x.nwk = nwk;
    }
}
static std::vector<std::uint64_t> g_unreachable;
static void zhac_adapter_unreachable(std::uint64_t ieee) { g_unreachable.push_back(ieee); }

#include "tx_delivery.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

constexpr std::uint64_t kPlug = 0xA4C1380000000001ull, kSensor = 0xA4C1380000000002ull, kOther = 0xA4C13800000000FFull;
constexpr std::uint8_t  kNoAck = 0xA7;
using Bytes = std::vector<std::uint8_t>;

// genOnOff On to `nwk`, each with its own TSN, as the adapter's frames.
static Bytes send_on(std::uint16_t nwk) {
    static std::uint8_t tsn = 0;
    const std::uint8_t f[3] = {0x01, ++tsn, 0x01};
    esp_zb_af_send(nwk, 1, 0x0006, f, sizeof(f));
    return Bytes(f, f + sizeof(f));
}
// The stack's confirm for `bytes` sent to `nwk`.
static void confirm(std::uint16_t nwk, Bytes bytes, std::uint8_t status, ezb_addr_mode_t mode = EZB_ADDR_MODE_SHORT) {
    ezb_apsde_data_confirm_t c{};
    c.dst_address.addr_mode = mode;
    c.dst_address.u.short_addr = nwk;
    c.cluster_id = 0x0006;
    c.profile_id = 0x0104;
    c.status = status;
    c.asdu_length = static_cast<std::uint16_t>(bytes.size());
    c.asdu = bytes.data();
    on_apsde_confirm(&c);
}
// A lookup ends: `ieee` answered from `nwk`, or nobody did (ieee 0: the stack's timeout).
static void answer(const ezb_zdo_nwk_addr_req_t& q, std::uint64_t ieee, std::uint16_t nwk) {
    if (!q.cb) return;
    ezb_zdp_address_rsp_field_t rsp{};
    rsp.ieee_addr_remote_dev.u64 = ieee;
    rsp.nwk_addr_remote_dev = nwk;
    ezb_zdo_nwk_addr_req_result_t r{};
    r.error = ieee ? 0 : 0x10;
    r.rsp = ieee ? &rsp : nullptr;
    q.cb(&r, q.user_ctx);
}
static void last_lookup_ends(std::uint64_t ieee, std::uint16_t nwk) {
    if (!g_lookups.empty()) answer(g_lookups.back(), ieee, nwk);
}
static void later() {   // past the broadcast budget's window
    g_now_us += 10'000'000;
    g_unreachable.clear();
    g_moved.clear();
}

int main() {
    g_pool = {{kPlug, 0x1234, false}, {kSensor, 0x2222, true}};
    g_map = {{0x1234, kPlug}, {0x2222, kSensor}};

    Bytes f = send_on(0x1234);
    confirm(0x1234, f, 0);
    expect("delivered: no address lookup", g_lookups.empty());

    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    expect("a mains device that did not take a frame is looked up: one NWK_addr_req", g_lookups.size() == 1);
    expect("  broadcast to the devices that keep their receiver on (0xFFFD), for its IEEE, that device only",
           !g_lookups.empty() && g_lookups.back().dst_nwk_addr == 0xFFFD &&
               g_lookups.back().field.ieee_addr_of_interest.u64 == kPlug && g_lookups.back().field.request_type == 0);
    std::size_t sent = g_sent.size();
    last_lookup_ends(kPlug, 0x1234);
    expect("found where it was: the same frame again, to the same address and endpoint, once",
           g_sent.size() == sent + 1 && g_sent.back().bytes == f && g_sent.back().nwk == 0x1234 &&
               g_sent.back().ep == 1 && g_sent.back().cluster == 0x0006 && g_moved.empty());
    confirm(0x1234, f, kNoAck);
    expect("  the resend failing too: no second lookup, not offline", g_lookups.size() == 1 && g_unreachable.empty());

    later();
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    last_lookup_ends(kPlug, 0x5678);
    expect("moved: the pool learns the new address",
           g_moved.size() == 1 && g_moved[0].first == kPlug && g_moved[0].second == 0x5678);
    expect("  and the frame goes there", g_sent.back().nwk == 0x5678 && g_sent.back().bytes == f);
    confirm(0x5678, f, 0);
    g_pool[0].nwk = 0x1234;

    later();
    sent = g_sent.size();
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    last_lookup_ends(0, 0);
    expect("nobody answered: offline (the adapter's unreachable)", g_unreachable.size() == 1 && g_unreachable[0] == kPlug);
    expect("  and nothing resent", g_sent.size() == sent + 1);

    later();
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    last_lookup_ends(kOther, 0x7777);
    expect("an answer for another device counts as none",
           g_unreachable.size() == 1 && g_unreachable[0] == kPlug && g_moved.empty());

    later();
    std::size_t looks = g_lookups.size();
    f = send_on(0x2222);
    confirm(0x2222, f, kNoAck);
    expect("a sleepy device: no lookup (the adapter holds its writes for its wake)", g_lookups.size() == looks);
    expect("  and never offline from here", g_unreachable.empty());
    f = send_on(0x9999);
    confirm(0x9999, f, kNoAck);
    expect("an address the pool does not know: no lookup", g_lookups.size() == looks);

    later();
    g_map.clear();   // the stack's address map forgot 0x1234
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    expect("the device the frame was for (the pool's) is looked up, also when the stack's map forgot the address",
           g_lookups.size() == looks + 1 && g_lookups.back().field.ieee_addr_of_interest.u64 == kPlug);
    last_lookup_ends(kPlug, 0x1234);
    confirm(0x1234, f, 0);
    g_map = {{0x1234, kPlug}, {0x2222, kSensor}};

    later();
    for (std::uint16_t i = 0; i < 5; i++) g_pool.push_back({kPlug + 0x100 + i, static_cast<std::uint16_t>(0x3001 + i), false});
    looks = g_lookups.size();
    std::vector<Bytes> fs;
    for (std::uint16_t i = 0; i < 5; i++) fs.push_back(send_on(0x3001 + i));
    for (std::uint16_t i = 0; i < 5; i++) confirm(0x3001 + i, fs[i], kNoAck);
    expect("five devices failing at once: four lookups (the broadcast budget)", g_lookups.size() == looks + 4);
    expect("  the fifth is not marked offline for it", g_unreachable.empty());
    for (std::size_t i = looks; i < g_lookups.size(); i++) answer(g_lookups[i], g_lookups[i].field.ieee_addr_of_interest.u64, 0x3001 + (i - looks));
    later();
    f = send_on(0x3005);
    confirm(0x3005, f, kNoAck);
    expect("  9 s later the budget is back", g_lookups.size() == looks + 5);
    last_lookup_ends(kPlug + 0x104, 0x3005);

    later();
    looks = g_lookups.size();
    g_aps_err = EZB_ERR_NO_MEM;
    const std::uint8_t r1[3] = {0x01, 0xF0, 0x01};
    expect("the stack refused the request: not sent", !esp_zb_af_send(0x1234, 1, 0x0006, r1, 3));
    g_aps_err = 0;
    confirm(0x1234, Bytes(r1, r1 + 3), kNoAck);
    expect("  and nothing waits for its confirm", g_lookups.size() == looks);
    zb_lock::g_ok = false;
    const std::uint8_t r2[3] = {0x01, 0xF1, 0x01};
    expect("the stack's lock not taken: not sent", !esp_zb_af_send(0x1234, 1, 0x0006, r2, 3));
    zb_lock::g_ok = true;
    confirm(0x1234, Bytes(r2, r2 + 3), kNoAck);
    expect("  and nothing waits for its confirm", g_lookups.size() == looks);

    later();
    g_zdo_err = 1;
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    g_zdo_err = 0;
    sent = g_sent.size();
    last_lookup_ends(kPlug, 0x1234);   // the stack would never call back; nothing may wait for it
    expect("a lookup the stack refused: the frame is let go, not offline",
           g_lookups.size() == looks + 1 && g_sent.size() == sent && g_unreachable.empty());

    later();
    looks = g_lookups.size();
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck, EZB_ADDR_MODE_GROUP);
    expect("a groupcast's confirm is not looked at", g_lookups.size() == looks);

    // Collections (B5): one groupcast instead of a frame per member.
    later();
    std::size_t before = g_sent.size();
    expect("a collection's On: sent", esp_zb_groupcast_onoff(101, 1));
    const Sent gc = g_sent.empty() ? Sent{} : g_sent.back();
    expect("  one frame, to group 101, for the devices that keep their receiver on (0xFFFD)",
           g_sent.size() == before + 1 && gc.mode == EZB_ADDR_MODE_GROUP && gc.nwk == 101 && gc.bcast == 0xFFFD);
    expect("  genOnOff from endpoint 1, Home Automation, no APS ACK (a groupcast has none)",
           gc.cluster == 0x0006 && gc.src_ep == 1 && gc.profile == 0x0104 && gc.tx_options == 0);
    expect("  the ZCL On: a cluster command, no default response", gc.bytes == Bytes({0x11, g_tsn, 0x01}));
    esp_zb_groupcast_onoff(101, 0);
    esp_zb_groupcast_onoff(101, 2);
    expect("Off and Toggle are commands 0 and 2, each with a TSN of its own",
           g_sent.size() == before + 3 && g_sent[before + 1].bytes[2] == 0x00 && g_sent[before + 2].bytes[2] == 0x02 &&
               g_sent[before + 1].bytes[1] != g_sent[before + 2].bytes[1] && g_sent[before + 1].bytes[1] != gc.bytes[1]);
    esp_zb_groupcast_onoff(101, 1);   // the fourth broadcast in 9 s
    before = g_sent.size();
    expect("a fifth broadcast within 9 s: refused, nothing sent", !esp_zb_groupcast_onoff(101, 1) && g_sent.size() == before);
    looks = g_lookups.size();
    f = send_on(0x1234);
    confirm(0x1234, f, kNoAck);
    expect("  the same budget as the address lookups: a failed frame is not looked up meanwhile", g_lookups.size() == looks);
    later();
    before = g_sent.size();
    expect("command 3, or group 0: refused before the radio",
           !esp_zb_groupcast_onoff(101, 3) && !esp_zb_groupcast_onoff(0, 1) && g_sent.size() == before);
    bool four = true;
    for (int i = 0; i < 4; i++) four = esp_zb_groupcast_onoff(102, 1) && four;
    expect("  and they spent no broadcast: four more go out", four && g_sent.size() == before + 4);
    later();
    g_aps_err = 5;
    expect("the stack refused it: false (the caller sends one command per member)", !esp_zb_groupcast_onoff(101, 1));
    g_aps_err = 0;
    zb_lock::g_ok = false;
    before = g_sent.size();
    expect("the stack's lock not taken: false, nothing sent", !esp_zb_groupcast_onoff(101, 1) && g_sent.size() == before);
    zb_lock::g_ok = true;

    // The stack hands a groupcast to the hub's own endpoint too when it is in the group, which it is for every
    // group a device joined: from 0x0000, or from the hub's IEEE.
    ezb_address_t src{};
    src.addr_mode = EZB_ADDR_MODE_SHORT;
    src.u.short_addr = 0x0000;
    expect("a frame from 0x0000 is the hub's own", from_self(src));
    src.u.short_addr = 0x1234;
    expect("  a device's is not", !from_self(src));
    src.addr_mode = EZB_ADDR_MODE_EXT;
    src.u.extended_addr.u64 = kCoord;
    expect("from the hub's IEEE: its own", from_self(src));
    src.u.extended_addr.u64 = kPlug;
    expect("  a device's IEEE: not", !from_self(src));
    g_coord = 0;
    src.u.extended_addr.u64 = 0;
    expect("  the hub's IEEE not known yet: no IEEE source is taken for it", !from_self(src));
    g_coord = kCoord;

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
