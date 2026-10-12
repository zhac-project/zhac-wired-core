// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_netmap -- the network map scan (spec 2026-10-12 §3). One worker task,
// created on the first scan and parked between scans, asks the routers with
// Mgmt_Lqi_req; esp_zb_netmap_walk decides what to ask, esp_zb_netmap_json
// builds the result. This file only talks to the stack, the pool and the
// listener.

#include "esp_zb_netmap.h"

#include "esp_zb_lock.h"
#include "esp_zb_lqi.h"
#include "esp_zb_netmap_json.h"
#include "esp_zb_netmap_walk.h"
#include "esp_zigbee_backend.h"   // wall clock, network_up

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ezbee/error.h"
#include "ezbee/nwk.h"
#include "ezbee/zdo/zdo_nwk_mgmt.h"
#include "ezbee/zdo/zdo_type.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "zap_common.h"
#include "zhac_task.h"
#include "zigbee_pool.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

using namespace zhc_zb::netmap;

const char* TAG = "zb_netmap";

constexpr uint32_t kPageTimeoutMs = 5000;   // spec §3.2.3
constexpr uint32_t kGapMs         = 100;    // between requests: gentle on the mesh
constexpr uint32_t kTaskStack     = 6144;   // PSRAM when the build allows (zhac_task_create)
constexpr size_t   kPageMax       = 16;     // records kept from one answer (2-3 fit in a frame)
constexpr size_t   kLocalMax      = 128;    // coordinator neighbour-table rows read

// Documents in PSRAM: the map is ~10-15 KB of JSON for 22 devices and internal
// RAM is the tight budget (same allocator as main/metrics_mqtt.cpp).
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
PsramAllocator s_alloc;

SemaphoreHandle_t s_mu        = nullptr;   // guards the status block below
SemaphoreHandle_t s_start_sem = nullptr;   // given by esp_zb_netmap_start
TaskHandle_t      s_task      = nullptr;
NetmapListener    s_listener  = nullptr;

// Status block (under s_mu).
bool     s_scanning = false;
uint32_t s_done = 0, s_total = 0;
char     s_current[30] = {};
char*    s_map = nullptr;     // last map JSON (PSRAM), nullptr before the first scan
size_t   s_map_len = 0;

// The page in flight. on_lqi copies an answer only when its ctx is the current
// token, and wakes the worker with a notification carrying that token, so a
// late answer to an ask that already timed out is dropped (spec §3.4).
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_token = 0;         // under s_mux; 0 = nothing in flight
uint32_t s_token_seq = 0;     // worker only
bool     s_page_ok = false;
uint8_t  s_page_total = 0, s_page_start = 0;
size_t   s_page_n = 0;
Neighbor s_page[kPageMax];

void lock()   { xSemaphoreTake(s_mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mu); }

void emit(const char* event, const JsonDocument& doc) {
    char buf[192];
    const size_t n = serializeJson(doc, buf, sizeof(buf));
    if (s_listener && n > 0 && n < sizeof(buf)) s_listener(event, buf, n);
}

// Stack task.
void on_lqi(const ezb_zdo_nwk_mgmt_lqi_req_result_t* r, void* ctx) {
    const uint32_t tok = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    bool mine;
    taskENTER_CRITICAL(&s_mux);
    mine = tok != 0 && tok == s_token;
    if (mine) {
        s_token = 0;
        s_page_ok = r && r->error == EZB_ERR_NONE && r->rsp && r->rsp->status == EZB_ZDP_STATUS_SUCCESS;
        s_page_n = 0;
        if (s_page_ok) {
            const auto* rsp = r->rsp;
            s_page_total = rsp->neighbor_table_entries;
            s_page_start = rsp->start_index;
            const size_t n = rsp->neighbor_table_list ? rsp->neighbor_table_list_count : 0;
            for (size_t i = 0; i < n && i < kPageMax; ++i) {
                const auto& e = rsp->neighbor_table_list[i];
                s_page[i] = Neighbor{e.extended_pan_id.u64, e.extended_addr.u64, e.nwk_addr,
                                     static_cast<uint8_t>(e.device_type), static_cast<uint8_t>(e.affinity),
                                     e.device_depth, e.lqa};
                s_page_n = i + 1;
            }
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (mine) xTaskNotify(s_task, tok, eSetValueWithOverwrite);
}

// One page; false on a refused request, an error status or no answer in 5 s.
bool ask_page(const Ask& a) {
    if (++s_token_seq == 0) ++s_token_seq;   // 0 means "nothing in flight"
    const uint32_t tok = s_token_seq;
    taskENTER_CRITICAL(&s_mux);
    s_token = tok;
    taskEXIT_CRITICAL(&s_mux);
    xTaskNotifyStateClear(nullptr);
    ulTaskNotifyValueClear(nullptr, UINT32_MAX);

    ezb_zdo_nwk_mgmt_lqi_req_t req{};
    req.dst_nwk_addr = a.nwk;
    req.field.start_index = a.start_index;
    req.cb = on_lqi;
    req.user_ctx = reinterpret_cast<void*>(static_cast<uintptr_t>(tok));
    bool sent;
    {
        // Lock only around the request: the answer comes on the stack task,
        // which needs the lock back (the esp_zb_interview.cpp pattern).
        zb_lock::Guard g;
        sent = g && ezb_zdo_nwk_mgmt_lqi_req(&req) == EZB_ERR_NONE;
    }
    if (sent) {
        const int64_t end = esp_timer_get_time() + int64_t(kPageTimeoutMs) * 1000;
        for (int64_t left = end - esp_timer_get_time(); left > 0; left = end - esp_timer_get_time()) {
            uint32_t v = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &v, pdMS_TO_TICKS(left / 1000 + 1)) == pdTRUE && v == tok)
                return s_page_ok;
        }
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_token == tok) s_token = 0;
    taskEXIT_CRITICAL(&s_mux);
    return false;
}

void publish_progress(const Walk& w, const Ask& a) {
    const Known* k = w.find_known(a.ieee);
    lock();
    s_done = w.routers_done();
    s_total = w.routers_total();
    if (k && k->name[0]) std::snprintf(s_current, sizeof(s_current), "%s", k->name);
    else                 std::snprintf(s_current, sizeof(s_current), "0x%016" PRIX64, a.ieee);
    JsonDocument doc(&s_alloc);
    doc["done"] = s_done;
    doc["total"] = s_total;
    doc["current"] = s_current;   // char[]: copied
    unlock();
    emit("net.map.progress", doc);
}

void finish(bool ok, const char* err, char* map, size_t map_len) {
    lock();
    if (ok) {
        heap_caps_free(s_map);
        s_map = map;
        s_map_len = map_len;
    }
    s_scanning = false;
    s_current[0] = '\0';
    unlock();
    JsonDocument doc(&s_alloc);
    doc["ts"] = esp_zigbee_backend_wall_clock_s();
    doc["ok"] = ok;
    if (!ok) doc["err"] = err;   // a literal
    emit("net.map.done", doc);
}

void run_scan() {
    const int64_t t0 = esp_timer_get_time();
    auto* known = static_cast<Known*>(heap_caps_calloc(ZAP_MAX_DEVICES, sizeof(Known), MALLOC_CAP_SPIRAM));
    auto* local = static_cast<Neighbor*>(heap_caps_calloc(kLocalMax, sizeof(Neighbor), MALLOC_CAP_SPIRAM));
    void* wmem = heap_caps_malloc(sizeof(Walk), MALLOC_CAP_SPIRAM);
    if (!known || !local || !wmem) {
        heap_caps_free(known); heap_caps_free(local); heap_caps_free(wmem);
        finish(false, "oom", nullptr, 0);
        return;
    }
    Walk* w = new (wmem) Walk();

    // The hub's devices: names, models, routers to fall back on, not_seen.
    size_t n_known = 0;
    zigbee_pool_lock();
    const ZapDevice* all = pool_all();
    const uint16_t count = pool_count();
    for (uint16_t i = 0; i < count && n_known < ZAP_MAX_DEVICES; ++i) {
        const ZapDevice& d = all[i];
        if (zap_dev_is_removed(&d) || d.protocol != PROTO_ZIGBEE) continue;
        Known& k = known[n_known++];
        k.ieee = d.ieee_addr;
        k.nwk = d.nwk_addr;
        k.type = d.device_type;
        k.last_seen = d.last_seen;
        std::snprintf(k.name, sizeof(k.name), "%.*s", int(sizeof(k.name) - 1), d.friendly_name);
        std::snprintf(k.model, sizeof(k.model), "%.*s", int(sizeof(k.model) - 1), d.model_id);
    }
    zigbee_pool_unlock();

    ezb_extaddr_t me{};
    ezb_extpanid_t xpan{};
    size_t n_local = 0;
    {
        zb_lock::Guard g;
        if (g) {
            ezb_nwk_get_extended_address(&me);
            ezb_nwk_get_extended_panid(&xpan);
            ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
            ezb_nwk_neighbor_info_t nb{};
            while (n_local < kLocalMax && ezb_nwk_get_next_neighbor(&it, &nb) == EZB_ERR_NONE) {
                local[n_local++] = Neighbor{0, nb.ieee_addr.u64, nb.short_addr, nb.device_type,
                                            nb.relationship, nb.depth, zhc_zb::lqi_from_rssi(nb.rssi)};
            }
        }
    }
    w->begin(me.u64, xpan.u64, known, n_known);
    w->add_local(local, n_local);

    Ask a{};
    while (w->next_ask(&a)) {
        publish_progress(*w, a);
        if (ask_page(a)) w->on_page(a, s_page_total, s_page_start, s_page, s_page_n);
        else             w->on_fail(a);
        vTaskDelay(pdMS_TO_TICKS(kGapMs));
    }

    const uint32_t dur_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "scan done: %u nodes, %u links, %u routers, %" PRIu32 " ms%s",
             unsigned(w->node_count()), unsigned(w->link_count()), unsigned(w->routers_total()), dur_ms,
             w->truncated() ? " (truncated)" : "");
    char* out = nullptr;
    size_t len = 0;
    {
        JsonDocument doc(&s_alloc);
        build_json(doc, *w, esp_zigbee_backend_wall_clock_s(), dur_ms);
        if (!doc.overflowed()) {
            len = measureJson(doc);
            out = static_cast<char*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM));
            if (out) serializeJson(doc, out, len + 1);
        }
    }
    w->~Walk();
    heap_caps_free(wmem);
    heap_caps_free(local);
    heap_caps_free(known);
    if (out) finish(true, nullptr, out, len);
    else     finish(false, "oom", nullptr, 0);
}

void task_netmap(void*) {
    for (;;) {
        xSemaphoreTake(s_start_sem, portMAX_DELAY);
        run_scan();
    }
}

bool init_once() {
    s_mu = xSemaphoreCreateMutex();
    s_start_sem = xSemaphoreCreateBinary();
    if (!s_mu || !s_start_sem) return false;
    if (zhac_task_create(task_netmap, "TaskZbMap", kTaskStack, nullptr, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "TaskZbMap create failed");
        return false;
    }
    return true;
}

}  // namespace

const char* esp_zb_netmap_start(void) {
    if (!esp_zigbee_backend_network_up()) return "not_ready";
    static const bool ready = init_once();   // first scan creates the task
    if (!ready) return "oom";
    lock();
    if (s_scanning) { unlock(); return "busy"; }
    s_scanning = true;
    s_done = s_total = 0;
    s_current[0] = '\0';
    unlock();
    xSemaphoreGive(s_start_sem);
    return nullptr;
}

char* esp_zb_netmap_get_json(size_t* len) {
    static constexpr char kEmpty[] = "{\"scanning\":false,\"progress\":{\"done\":0,\"total\":0,\"current\":\"\"},\"map\":null}";
    if (!s_mu) {   // no scan ever started
        char* b = static_cast<char*>(heap_caps_malloc(sizeof(kEmpty), MALLOC_CAP_SPIRAM));
        if (b) { std::memcpy(b, kEmpty, sizeof(kEmpty)); *len = sizeof(kEmpty) - 1; }
        return b;
    }
    lock();
    JsonDocument p(&s_alloc);
    p["done"] = s_done;
    p["total"] = s_total;
    p["current"] = s_current;   // char[]: copied
    char prog[160];
    const size_t pn = serializeJson(p, prog, sizeof(prog));
    const size_t cap = 64 + pn + (s_map ? s_map_len : 4);
    char* buf = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (buf) {
        size_t n = static_cast<size_t>(std::snprintf(buf, cap, "{\"scanning\":%s,\"progress\":%.*s,\"map\":",
                                                      s_scanning ? "true" : "false", int(pn), prog));
        if (s_map) { std::memcpy(buf + n, s_map, s_map_len); n += s_map_len; }
        else       { std::memcpy(buf + n, "null", 4); n += 4; }
        buf[n++] = '}';
        buf[n] = '\0';
        *len = n;
    }
    unlock();
    return buf;
}

void esp_zb_netmap_set_listener(NetmapListener fn) {
    s_listener = fn;
}
