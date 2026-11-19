// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the radio-job worker (research 2026-11-18 B2), against the REAL handlers, worker and replies cut
// out of main/ws_bridge.cpp (ws_slice.inc, see run.sh). The web server's task is this program's main thread.
// Exit code = number of mismatches.
#include <ArduinoJson.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "json_buf.h"   // the real JsonWriter (main/)

using namespace std::chrono;

// ── FreeRTOS on std::thread: 1 tick = 100 ns ─────────────────────────────────
using TickType_t  = uint32_t;
using BaseType_t  = int;
using UBaseType_t = unsigned;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static nanoseconds ticks(TickType_t t) { return nanoseconds(int64_t(t) * 100); }

struct FakeTask { const char* name; };
using TaskHandle_t = FakeTask*;
static FakeTask g_httpd{"httpd"};
static thread_local TaskHandle_t t_self = nullptr;
static std::atomic<int>  g_started{0}, g_ended{0};
static std::atomic<bool> g_create_fail{false};
struct TaskEnd {};
static TaskHandle_t xTaskGetCurrentTaskHandle() { return t_self ? t_self : &g_httpd; }
static BaseType_t xTaskCreate(void (*fn)(void*), const char* name, uint32_t, void* arg, UBaseType_t,
                              TaskHandle_t* out) {
    if (g_create_fail) return pdFALSE;
    static std::mutex           m;
    static std::deque<FakeTask> all;   // kept to the end: a handle may be compared after its task ended
    FakeTask* t;
    {
        std::lock_guard<std::mutex> l(m);
        all.push_back({name});
        t = &all.back();
    }
    if (out) *out = t;
    g_started++;
    std::thread([fn, arg, t] {
        t_self = t;
        try {
            fn(arg);
        } catch (const TaskEnd&) {
        }
        g_ended++;
    }).detach();
    return pdPASS;
}
static void vTaskDelete(TaskHandle_t t) {
    if (!t) throw TaskEnd{};
}

struct FakeQueue {
    std::mutex                        m;
    std::condition_variable           cv;
    std::deque<std::vector<uint8_t>> items;
    size_t                            depth = 0, size = 0;
};
using QueueHandle_t = FakeQueue*;
static QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t size) {
    auto* q = new FakeQueue;
    q->depth = depth;
    q->size = size;
    return q;
}
static BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t) {
    std::lock_guard<std::mutex> l(q->m);
    if (q->items.size() >= q->depth) return pdFALSE;
    const auto* p = static_cast<const uint8_t*>(item);
    q->items.emplace_back(p, p + q->size);
    q->cv.notify_one();
    return pdTRUE;
}
// Runs once on the receiving task right after a receive timed out (before it returns): a job posted in the
// moment the worker decides whether to end.
static std::mutex            g_idle_m;
static std::function<void()> g_on_idle;
static BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t t) {
    std::unique_lock<std::mutex> l(q->m);
    const auto ready = [q] { return !q->items.empty(); };
    if (t == portMAX_DELAY) {
        q->cv.wait(l, ready);
    } else if (!q->cv.wait_for(l, ticks(t), ready)) {
        l.unlock();
        std::function<void()> f;
        {
            std::lock_guard<std::mutex> il(g_idle_m);
            f.swap(g_on_idle);
        }
        if (f) f();
        return pdFALSE;
    }
    std::memcpy(item, q->items.front().data(), q->size);
    q->items.pop_front();
    return pdTRUE;
}
static UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    std::lock_guard<std::mutex> l(q->m);
    return static_cast<UBaseType_t>(q->items.size());
}
struct FakeMutex { std::mutex m; };
using SemaphoreHandle_t = FakeMutex*;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeMutex; }
static BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
    s->m.lock();
    return pdTRUE;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    s->m.unlock();
    return pdTRUE;
}

// ── the WebSocket server: replies, and which sockets are still signed-in WebSocket clients ──
struct Reply { int fd; std::string text; };
static std::mutex              g_reply_m;
static std::condition_variable g_reply_cv;
static std::vector<Reply>      g_replies;
static std::set<int>           g_live = {7};
static void ws_server_reply(int fd, const char* buf, size_t len) {
    std::lock_guard<std::mutex> l(g_reply_m);
    g_replies.push_back({fd, std::string(buf, len)});
    g_reply_cv.notify_all();
}
static bool ws_server_fd_is_authed(int fd) {
    std::lock_guard<std::mutex> l(g_reply_m);
    return g_live.count(fd) != 0;
}
static void close_socket(int fd) {
    std::lock_guard<std::mutex> l(g_reply_m);
    g_live.erase(fd);
}

// ── the pool ─────────────────────────────────────────────────────────────────
constexpr uint64_t kPlug = 0xA4C1380000000001ull, kCoord = 0x00124B0000000C00ull;
struct ZapDevice { uint64_t ieee_addr; uint16_t nwk_addr; };
static ZapDevice g_dev{kPlug, 0x1234};
static void       zigbee_pool_lock() {}
static void       zigbee_pool_unlock() {}
static ZapDevice* pool_find_by_ieee(uint64_t ieee) { return ieee == g_dev.ieee_addr ? &g_dev : nullptr; }
static bool zigbee_pool_snapshot(uint64_t ieee, ZapDevice* out) {
    if (ieee != g_dev.ieee_addr) return false;
    *out = g_dev;
    return true;
}
static uint64_t zigbee_mgr_coordinator_ieee() { return kCoord; }

// ── the radio: every call waits until the test lets the device answer (or 300 ms) ──
struct Call { std::string what; TaskHandle_t task; uint16_t nwk; uint8_t ep; uint16_t arg; uint64_t ieee, coord; };
static std::mutex              g_radio_m;
static std::condition_variable g_radio_cv;
static std::vector<Call>       g_calls;
static int                     g_answers = 0, g_in_radio = 0, g_max_in_radio = 0;
static bool                    g_radio_ok = true;
static std::function<void()>   g_during_radio;   // runs on the radio's task while the device "thinks"
static std::vector<uint16_t>   g_device_groups = {5, 6};
static bool radio(const char* what, uint16_t nwk, uint8_t ep, uint16_t arg, uint64_t ieee, uint64_t coord) {
    std::function<void()> during;
    {
        std::unique_lock<std::mutex> l(g_radio_m);
        g_calls.push_back({what, xTaskGetCurrentTaskHandle(), nwk, ep, arg, ieee, coord});
        g_max_in_radio = std::max(g_max_in_radio, ++g_in_radio);
        g_radio_cv.notify_all();
        during = g_during_radio;
    }
    if (during) during();
    std::unique_lock<std::mutex> l(g_radio_m);
    g_radio_cv.wait_for(l, milliseconds(300), [] { return g_answers > 0; });
    if (g_answers > 0) g_answers--;
    g_in_radio--;
    return g_radio_ok;
}
static bool zigbee_zdo_bind(uint16_t n, uint64_t ie, uint8_t ep, uint16_t cl, uint64_t dst, uint8_t) {
    return radio("bind", n, ep, cl, ie, dst);
}
static bool zigbee_zdo_unbind(uint16_t n, uint64_t ie, uint8_t ep, uint16_t cl, uint64_t dst, uint8_t) {
    return radio("unbind", n, ep, cl, ie, dst);
}
static bool zigbee_zcl_group_add(uint16_t n, uint8_t ep, uint16_t gid) { return radio("add", n, ep, gid, 0, 0); }
static bool zigbee_zcl_group_remove(uint16_t n, uint8_t ep, uint16_t gid) { return radio("remove", n, ep, gid, 0, 0); }
static bool zigbee_zcl_get_group_membership(uint16_t n, uint8_t ep, uint16_t* out, uint8_t max, uint8_t* count) {
    const bool ok = radio("membership", n, ep, 0, 0, 0);
    *count = 0;
    for (uint16_t g : g_device_groups) {
        if (*count < max) out[(*count)++] = g;
    }
    return ok;
}

// ── the group mirror (dgm_store) ────────────────────────────────────────────
static constexpr uint8_t DGM_MAX_GIDS = 16;
static std::mutex                                          g_dgm_m;
static std::map<std::pair<uint64_t, uint8_t>, std::vector<uint16_t>> g_dgm;
static bool dgm_add(uint64_t ieee, uint8_t ep, uint16_t gid) {
    std::lock_guard<std::mutex> l(g_dgm_m);
    auto& v = g_dgm[{ieee, ep}];
    if (std::find(v.begin(), v.end(), gid) == v.end()) v.push_back(gid);
    return true;
}
static bool dgm_remove(uint64_t ieee, uint8_t ep, uint16_t gid) {
    std::lock_guard<std::mutex> l(g_dgm_m);
    auto& v = g_dgm[{ieee, ep}];
    v.erase(std::remove(v.begin(), v.end(), gid), v.end());
    return true;
}
static bool dgm_set(uint64_t ieee, uint8_t ep, const uint16_t* gids, uint8_t count) {
    std::lock_guard<std::mutex> l(g_dgm_m);
    g_dgm[{ieee, ep}].assign(gids, gids + count);
    return true;
}
static uint8_t dgm_list(uint64_t ieee, uint16_t* out, uint8_t max) {
    std::lock_guard<std::mutex> l(g_dgm_m);
    uint8_t n = 0;
    for (const auto& [k, v] : g_dgm) {
        if (k.first != ieee) continue;
        for (uint16_t g : v) {
            if (n < max) out[n++] = g;
        }
    }
    return n;
}
static std::vector<uint16_t> mirror() {
    std::lock_guard<std::mutex> l(g_dgm_m);
    return g_dgm[{kPlug, 1}];
}

#include "ws_slice.inc"

// ── the checks ───────────────────────────────────────────────────────────────
static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static JsonDocument req(const char* json) {
    JsonDocument d;
    deserializeJson(d, json);
    return d;
}
static size_t replies() {
    std::lock_guard<std::mutex> l(g_reply_m);
    return g_replies.size();
}
static Reply reply(size_t i) {
    std::lock_guard<std::mutex> l(g_reply_m);
    return i < g_replies.size() ? g_replies[i] : Reply{-1, ""};
}
static bool wait_replies(size_t n, int ms = 2000) {
    std::unique_lock<std::mutex> l(g_reply_m);
    return g_reply_cv.wait_for(l, milliseconds(ms), [n] { return g_replies.size() >= n; });
}
static bool wait_calls(size_t n, int ms = 2000) {
    std::unique_lock<std::mutex> l(g_radio_m);
    return g_radio_cv.wait_for(l, milliseconds(ms), [n] { return g_calls.size() >= n; });
}
static Call call(size_t i) {
    std::lock_guard<std::mutex> l(g_radio_m);
    return i < g_calls.size() ? g_calls[i] : Call{};
}
static void answer(int n = 1) {   // the device answers the next n radio calls
    std::lock_guard<std::mutex> l(g_radio_m);
    g_answers += n;
    g_radio_cv.notify_all();
}
static bool workers_ended(int ms = 1000) {   // the worker's idle end is 6 ms here
    const auto until = steady_clock::now() + milliseconds(ms);
    while (steady_clock::now() < until) {
        if (g_started == g_ended) return true;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}
static void reset() {
    workers_ended();
    {
        std::lock_guard<std::mutex> l(g_reply_m);
        g_replies.clear();
        g_live = {7};
    }
    std::lock_guard<std::mutex> l(g_radio_m);
    g_calls.clear();
    g_answers = 0;
    g_radio_ok = true;
    g_during_radio = nullptr;
}
static long ms_since(steady_clock::time_point t0) {
    return static_cast<long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}
#define PLUG "\"ieee\":\"0xA4C1380000000001\""

int main() {
    // device.bind
    auto t0 = steady_clock::now();
    JsonDocument d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6}}");
    cmd_device_bind(7, 101, d);
    const long took = ms_since(t0);
    expect("device.bind: the web server's task is back at once", took < 100);
    expect("  with no answer yet", replies() == 0);
    wait_calls(1);
    const Call b = call(0);
    expect("  the bind goes out from another task, to the device, for its cluster, to the coordinator",
           b.what == "bind" && b.task != &g_httpd && b.nwk == 0x1234 && b.ep == 1 && b.arg == 6 && b.ieee == kPlug &&
               b.coord == kCoord);
    answer();
    expect("  answered when the device did: ok, its id, its socket",
           wait_replies(1) && reply(0).fd == 7 && reply(0).text == "{\"id\":101,\"ok\":true}");

    reset();
    d = req("{\"args\":{" PLUG ",\"cluster\":6}}");
    cmd_device_bind(7, 102, d);
    expect("a request that fails its checks is answered at once, without the radio",
           replies() == 1 && reply(0).text.find("\"ok\":false") != std::string::npos && call(0).what.empty());

    reset();
    g_radio_ok = false;
    d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6,\"unbind\":true}}");
    cmd_device_bind(7, 103, d);
    answer();
    expect("an unbind the device refused: \"unbind failed\"",
           wait_replies(1) && call(0).what == "unbind" && reply(0).text.find("unbind failed") != std::string::npos);

    // device.groups.add / remove / refresh
    reset();
    t0 = steady_clock::now();
    d = req("{\"args\":{" PLUG ",\"gid\":101,\"ep\":1}}");
    cmd_device_groups_add_remove(7, 104, d, false);
    expect("device.groups.add: back at once", ms_since(t0) < 100 && replies() == 0);
    answer();
    expect("  the device took it: mirrored, and the answer lists the groups",
           wait_replies(1) && reply(0).text == "{\"id\":104,\"ok\":true,\"data\":{\"groups\":[101]}}" &&
               mirror() == std::vector<uint16_t>{101});

    reset();
    g_radio_ok = false;
    d = req("{\"args\":{" PLUG ",\"gid\":102,\"ep\":1}}");
    cmd_device_groups_add_remove(7, 105, d, false);
    answer();
    expect("a group add the radio refused: \"command failed\", the mirror unchanged",
           wait_replies(1) && reply(0).text.find("command failed") != std::string::npos &&
               mirror() == std::vector<uint16_t>{101});

    reset();
    d = req("{\"args\":{" PLUG ",\"gid\":101,\"ep\":1}}");
    cmd_device_groups_add_remove(7, 106, d, true);
    answer();
    expect("device.groups.remove: mirrored, the answer lists what is left",
           wait_replies(1) && call(0).what == "remove" &&
               reply(0).text == "{\"id\":106,\"ok\":true,\"data\":{\"groups\":[]}}" && mirror().empty());

    reset();
    g_radio_ok = false;
    dgm_add(kPlug, 1, 9);
    d = req("{\"args\":{" PLUG ",\"ep\":1}}");
    cmd_device_groups_refresh(7, 107, d);
    answer();
    expect("a refresh the device did not answer: \"device did not answer\", the mirror left as it was",
           wait_replies(1) && reply(0).text.find("device did not answer") != std::string::npos &&
               mirror() == std::vector<uint16_t>{9});
    reset();
    d = req("{\"args\":{" PLUG ",\"ep\":1}}");
    cmd_device_groups_refresh(7, 108, d);
    answer();
    expect("a refresh answered: the mirror becomes the device's table",
           wait_replies(1) && reply(0).text == "{\"id\":108,\"ok\":true,\"data\":{\"groups\":[5,6]}}" &&
               mirror() == (std::vector<uint16_t>{5, 6}));

    // The page went away while the device answered; its socket's number may now carry a page request.
    reset();
    g_during_radio = [] { close_socket(7); };
    d = req("{\"args\":{" PLUG ",\"gid\":120,\"ep\":1}}");
    cmd_device_groups_add_remove(7, 109, d, false);
    answer();
    wait_calls(1);
    const auto until = steady_clock::now() + milliseconds(500);
    while (steady_clock::now() < until && mirror() != std::vector<uint16_t>{5, 6, 120})
        std::this_thread::sleep_for(milliseconds(1));
    std::this_thread::sleep_for(milliseconds(20));
    expect("the page went away meanwhile: no frame to its socket's number", replies() == 0);
    expect("  the mirror still follows the device", mirror() == (std::vector<uint16_t>{5, 6, 120}));

    reset();
    d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":8}}");
    cmd_device_bind(-32768, 110, d);   // REMOTE_VIRTUAL_FD: the cloud relay has no socket here
    answer();
    expect("the cloud relay's request (fd < 0) is answered from the worker",
           wait_replies(1) && reply(0).fd == -32768 && reply(0).text == "{\"id\":110,\"ok\":true}");

    // Queue and order.
    reset();
    g_max_in_radio = 0;
    for (uint32_t id = 201; id <= 206; id++) {
        d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6}}");
        cmd_device_bind(7, id, d);
        if (id == 201) wait_calls(1);   // the worker holds the first; four more fit in the queue
    }
    expect("one waiting and four queued: the sixth is answered busy at once",
           replies() == 1 && reply(0).text.find("\"id\":206,\"ok\":false") != std::string::npos &&
               reply(0).text.find("busy") != std::string::npos);
    answer(5);
    wait_replies(6);
    bool in_order = replies() == 6;
    for (size_t i = 1; i < 6 && in_order; i++)
        in_order = reply(i).text == "{\"id\":" + std::to_string(200 + i) + ",\"ok\":true}";
    expect("  the five go out one at a time, in order, each answered", in_order && g_max_in_radio == 1);

    // The worker's life.
    reset();
    expect("the worker ends after a minute without a job", workers_ended() && g_started > 0);
    const int before = g_started;
    d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6}}");
    cmd_device_bind(7, 301, d);
    answer();
    expect("  and the next command starts it again", wait_replies(1) && g_started == before + 1);

    reset();
    g_create_fail = true;
    d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6}}");
    cmd_device_bind(7, 302, d);
    g_create_fail = false;
    expect("no worker could be started: busy at once, nothing left waiting",
           replies() == 1 && reply(0).text.find("busy") != std::string::npos && !wait_calls(1, 50));

    // A job posted just as the worker found its queue empty and is deciding whether to end.
    reset();
    d = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6}}");
    cmd_device_bind(7, 401, d);
    answer();
    wait_replies(1);
    {
        std::lock_guard<std::mutex> l(g_idle_m);
        g_on_idle = [] {
            JsonDocument late = req("{\"args\":{" PLUG ",\"src_ep\":1,\"cluster\":6}}");
            cmd_device_bind(7, 402, late);
            answer();
        };
    }
    expect("a job queued as the worker decides to end is still sent and answered",
           wait_replies(2, 1000) && reply(1).text == "{\"id\":402,\"ok\":true}");

    expect("the worker is gone at the end", workers_ended());
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
