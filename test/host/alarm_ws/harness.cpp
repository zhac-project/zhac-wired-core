// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host check of the hub page's alarm.* commands (main/ws_bridge.cpp): only an absent id means "no id", so a null or
// empty one never disarms every profile (review W18), and alarm.get never answers a cut document as the whole
// config (W20). The real commands are in slice_tu.cpp; here are their neighbours: the counted allocator, the reply
// sink and the panel's calls, recorded.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "alarm.h"

void h_alarm_get(int fd, uint32_t id);
void h_alarm_arm(int fd, uint32_t id, JsonDocument& doc);
void h_alarm_disarm(int fd, uint32_t id, JsonDocument& doc);
void h_alarm_silence(int fd, uint32_t id, JsonDocument& doc);

static int g_fail = 0, g_checks = 0;
#define CHECK(c, m) do { g_checks++; if (!(c)) { std::printf("FAIL: %s (line %d)\n", (m), __LINE__); g_fail++; } } while (0)

// heap_caps: every call counted; call number g_fail_at (1-based) fails, 0 = none
static int g_allocs = 0, g_fail_at = 0;
static bool fails() { return ++g_allocs == g_fail_at; }
void* heap_caps_malloc(size_t n, uint32_t) { return fails() ? nullptr : std::malloc(n); }
void* heap_caps_realloc(void* p, size_t n, uint32_t) { return fails() ? nullptr : std::realloc(p, n); }
void heap_caps_free(void* p) { std::free(p); }

static std::vector<std::string> g_replies;
void ws_server_reply(int, const char* buf, size_t len) { g_replies.emplace_back(buf, len); }
struct ZapDevice;
bool zigbee_pool_snapshot(uint64_t, ZapDevice*) { return false; }

// The panel: four profiles, and what each call was given
static std::vector<std::string> g_calls;
static bool known(const char* id) {
    for (const char* p : {"away", "home", "night", "flood"}) if (std::strcmp(id, p) == 0) return true;
    return false;
}
AlarmReply alarm_arm(const char* id, bool, uint64_t*, uint8_t*) {
    g_calls.push_back(std::string("arm:") + id);
    return known(id) ? AlarmReply::OK : AlarmReply::NO_PROFILE;
}
AlarmReply alarm_disarm(const char* id) {   // "" is every profile
    g_calls.push_back(std::string("disarm:") + id);
    return !*id || known(id) ? AlarmReply::OK : AlarmReply::NO_PROFILE;
}
AlarmReply alarm_silence(const char* id) {
    g_calls.push_back(std::string("silence:") + id);
    return known(id) ? AlarmReply::OK : AlarmReply::NO_PROFILE;
}
static int g_allocs_filled = 0;   // the count once alarm.get's document is filled
void alarm_status_json(JsonObject o) {
    o["summary"] = std::string("armed_away");
    JsonArray a = o["profiles"].to<JsonArray>();
    for (const char* id : {"away", "home", "night", "flood"}) {
        JsonObject p = a.add<JsonObject>();
        p["id"] = std::string(id);
        p["state"] = std::string(id) + " armed";
    }
}
void alarm_config_json(JsonObject o) {
    JsonArray a = o["profiles"].to<JsonArray>();
    for (const char* id : {"away", "home", "night", "flood"}) {
        JsonObject p = a.add<JsonObject>();
        p["id"] = std::string(id);
        p["name"] = std::string(id) + " profile";
    }
    g_allocs_filled = g_allocs;
}

using Cmd = void (*)(int, uint32_t, JsonDocument&);
static std::string run(Cmd f, const char* req) {   // the one reply to a request of id 5
    JsonDocument doc;
    deserializeJson(doc, req);
    g_replies.clear();
    g_calls.clear();
    f(1, 5, doc);
    return g_replies.size() == 1 ? g_replies[0] : "(" + std::to_string(g_replies.size()) + " replies)";
}
using Calls = std::vector<std::string>;

int main() {
    const std::string no = R"({"id":5,"ok":false,"err":"no such profile"})", ok = R"({"id":5,"ok":true})";

    // ── W18: a null, empty or non-text id names no profile, for every command that takes one ──
    struct Named { const char* name; Cmd f; };
    for (const Named c : {Named{"arm", h_alarm_arm}, Named{"disarm", h_alarm_disarm}, Named{"silence", h_alarm_silence}}) {
        for (const char* req : {R"({"args":{"id":null}})", R"({"args":{"id":""}})", R"({"args":{"id":7}})",
                                R"({"args":{"id":true}})", R"({"args":{"id":["away"]}})", R"({"args":{"id":null,"mode":"away"}})"}) {
            const std::string what = std::string(c.name) + " " + req + ": no such profile, and the panel is not asked";
            CHECK(run(c.f, req) == no && g_calls.empty(), what.c_str());
        }
        const std::string what = std::string(c.name) + ": a profile's id reaches the panel";
        run(c.f, R"({"args":{"id":"night"}})");
        CHECK(g_calls == Calls({std::string(c.name) + ":night"}), what.c_str());
    }
    CHECK(run(h_alarm_disarm, R"({"args":{}})") == ok && g_calls == Calls({"disarm:"}), "disarm without an id: every profile");
    CHECK(run(h_alarm_disarm, "{}") == ok && g_calls == Calls({"disarm:"}), "disarm without args: every profile");
    CHECK(run(h_alarm_arm, R"({"args":{"mode":"home"}})") == R"({"id":5,"ok":true,"data":{"armed":true}})" &&
          g_calls == Calls({"arm:home"}), "arm without an id: stage 1's mode");
    CHECK(run(h_alarm_silence, R"({"args":{}})") == no && g_calls == Calls({"silence:"}), "silence without an id: the panel's answer");

    // ── W20: whichever allocation fails, in the document or in the reply, alarm.get answers an error ──
    g_allocs = 0;
    g_fail_at = 0;
    g_replies.clear();
    h_alarm_get(1, 5);
    const std::string whole = g_replies.size() == 1 ? g_replies[0] : "";
    const int total = g_allocs, filled = g_allocs_filled;
    CHECK(whole.rfind(R"({"id":5,"ok":true,"data":{"summary":"armed_away")", 0) == 0 && whole.find("flood profile") != std::string::npos,
          "alarm.get: the status and the whole config");
    CHECK(filled > 1 && total > filled + 1, "allocations in the document and in the reply");
    int cut = 0;
    for (int k = 1; k <= total; k++) {
        g_allocs = 0;
        g_fail_at = k;
        g_replies.clear();
        h_alarm_get(1, 5);
        const std::string r = g_replies.size() == 1 ? g_replies[0] : "(" + std::to_string(g_replies.size()) + " replies)";
        if (r == whole || r == R"({"id":5,"ok":false,"err":"oom"})") continue;
        cut++;
        std::printf("  allocation %d of %d (%s) fails: %s\n", k, total, k <= filled ? "document" : "reply", r.c_str());
    }
    g_fail_at = 0;
    CHECK(cut == 0, "a failed allocation is answered oom: never a cut config as the whole one");

    std::printf("%s (%d checks, %d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_checks, g_fail);
    return g_fail;
}
