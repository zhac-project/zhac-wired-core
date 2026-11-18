// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fakes for mqtt_glue.cpp's per-device state publisher: a one-device shadow, the MQTT client and the
// Home Assistant bridge. See run.sh.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "device_shadow.h"   // ShadowAttr (FreeRTOS from the shadow's host stubs)
#include "esp_heap_caps.h"   // the same stubs: malloc
#include "event_bus.h"       // Event, ZclAttrEvent
#include "ha_discovery.h"    // ha::state_retained, ha::x100_to_json

#define ESP_LOGW(tag, ...) ((void)(tag))
[[maybe_unused]] static const char* TAG = "mqtt_glue";

static std::vector<ShadowAttr> g_shadow;
uint8_t device_shadow_get_attrs(uint64_t, ShadowAttr* out, uint8_t max) {
    uint8_t n = 0;
    for (const ShadowAttr& a : g_shadow)
        if (n < max) out[n++] = a;
    return n;
}

struct Pub { std::string topic, payload; bool retain; };
static std::vector<Pub> g_pubs;
static bool g_connected = true;
bool mqtt_gw_is_connected() { return g_connected; }
int  mqtt_gw_format_topic(char* out, size_t cap, const char* suffix) { return snprintf(out, cap, "zhac/%s", suffix); }
bool mqtt_gw_publish(const char* topic, const char* payload, size_t n, int, bool retain) {
    g_pubs.push_back({topic, std::string(payload, n), retain});
    return true;
}
static int g_removed = 0;
void ha_bridge_publish_state(uint64_t, const char*, const char*) {}
void ha_bridge_device_removed(uint64_t) { g_removed++; }

#include "slice.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static const uint64_t kIeee = 0x00158D0007A1B2C3ULL;

static ShadowAttr attr(const char* key, uint8_t type, int32_t v, const char* s = nullptr) {
    ShadowAttr a{};
    std::strncpy(a.key, key, ATTR_KEY_MAX - 1);
    a.val_type = type;
    if (s) std::strncpy(a.str_val, s, ATTR_STR_MAX);
    else a.int_val = v;
    return a;
}
static Event report(const char* key, uint8_t type, int32_t v, bool more, const char* s = nullptr) {
    Event e{};
    e.type = EventType::ZCL_ATTR;
    auto& z = *reinterpret_cast<ZclAttrEvent*>(e.data);
    z.ieee = kIeee;
    std::strncpy(z.key, key, ATTR_KEY_MAX - 1);
    z.val_type = type;
    if (s) std::strncpy(z.str_val, s, ATTR_STR_MAX);
    else z.int_val = v;
    z.more = more;
    return e;
}
static JsonDocument parse(const Pub& p) {
    JsonDocument d;
    deserializeJson(d, p.payload);
    return d;
}

int main() {
    g_shadow = {attr("temperature", VAL_FLOAT, 2150), attr("state", VAL_BOOL, 1), attr("battery", VAL_INT, 90),
                attr("mode", VAL_STR, 0, "auto"), attr("_since", VAL_INT, 5), attr("action", VAL_STR, 0, "single"),
                attr("pin_code", VAL_STR, 0, "1234")};

    // A report of two attrs: one message, after the last, retained, the whole state.
    g_pubs.clear();
    on_attr(report("temperature", VAL_FLOAT, 2150, true));
    expect("no message before the report's last attr", g_pubs.empty());
    on_attr(report("battery", VAL_INT, 90, false));
    expect("one message per report", g_pubs.size() == 1);
    if (g_pubs.size() == 1) {
        const Pub& p = g_pubs[0];
        JsonDocument d = parse(p);
        JsonObjectConst a = d["attrs"];
        expect("on <root>/devices/<IEEE>/state", p.topic == "zhac/devices/00158D0007A1B2C3/state");
        expect("retained", p.retain);
        expect("the device's address", d["ieee"] == "0x00158D0007A1B2C3");
        expect("the whole state", a["temperature"] == 21.5 && a["state"] == 1 && a["battery"] == 90 &&
                                      a["mode"] == "auto");
        expect("no bookkeeping key", a["_since"].isNull());
        expect("no stale press in a retained state", a["action"].isNull());
        expect("no secret in a retained state", a["pin_code"].isNull());
    }

    // A press: at once, with the state, NOT retained -- then the retained state without it.
    g_pubs.clear();
    on_attr(report("action", VAL_STR, 0, false, "double"));
    expect("a press: two messages", g_pubs.size() == 2);
    if (g_pubs.size() == 2) {
        JsonDocument d0 = parse(g_pubs[0]), d1 = parse(g_pubs[1]);
        expect("  the press first, not retained, with the state",
               !g_pubs[0].retain && d0["attrs"]["action"] == "double" && d0["attrs"]["battery"] == 90);
        expect("  then the state, retained, without the press", g_pubs[1].retain && d1["attrs"]["action"].isNull());
    }
    g_pubs.clear();
    on_attr(report("action", VAL_STR, 0, true, "hold"));
    expect("a press mid-report goes out at once", g_pubs.size() == 1 && !g_pubs[0].retain);
    on_attr(report("battery", VAL_INT, 89, false));
    expect("  and the report's state after its last attr", g_pubs.size() == 2 && g_pubs[1].retain);

    // A bookkeeping key ending a report still ends it.
    g_pubs.clear();
    on_attr(report("_since", VAL_INT, 6, false));
    expect("a report ending in a bookkeeping key: its state, retained",
           g_pubs.size() == 1 && g_pubs[0].retain && parse(g_pubs[0])["attrs"]["_since"].isNull());

    g_pubs.clear();
    g_connected = false;
    on_attr(report("battery", VAL_INT, 88, false));
    expect("broker down: nothing", g_pubs.empty());
    g_connected = true;

    // A removed device: Home Assistant told, and its retained state cleared (an empty retained message),
    // as z2m does -- else every new subscriber would still see it.
    g_pubs.clear();
    Event leave{};
    leave.type = EventType::DEVICE_LEAVE;
    std::memcpy(leave.data, &kIeee, sizeof(kIeee));
    on_device_leave(leave);
    expect("removed: Home Assistant told", g_removed == 1);
    expect("removed: retained state cleared", g_pubs.size() == 1 && g_pubs[0].retain && g_pubs[0].payload.empty() &&
                                                  g_pubs[0].topic == "zhac/devices/00158D0007A1B2C3/state");

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
