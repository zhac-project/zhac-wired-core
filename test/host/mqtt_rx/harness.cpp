// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fakes for mqtt_glue.cpp's on_mqtt_rx(): the Home Assistant bridge (it takes /set) and the event bus. See run.sh.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "event_bus.h"
#include "mqtt_payload.h"

static std::vector<Event> g_events;
void event_bus_publish(const Event& e) { g_events.push_back(e); }
const char* mqtt_gw_get_root_topic() { return "zhac"; }
bool ha_bridge_on_mqtt_rx(const char* topic, int len, const char*, int) {
    return len > 4 && std::strncmp(topic + len - 4, "/set", 4) == 0;
}

#include "slice.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static bool rx(const std::string& topic, const std::string& data = "x") {
    g_events.clear();
    on_mqtt_rx(topic.data(), (int)topic.size(), data.data(), (int)data.size());
    return !g_events.empty();
}
static const MqttMsgEvent& last() { return *reinterpret_cast<const MqttMsgEvent*>(g_events.back().data); }

int main() {
    expect("a message for the rules and Lua", rx("zhac/kitchen/light", "ON") &&
                                                  std::strcmp(last().topic, "zhac/kitchen/light") == 0 &&
                                                  std::strcmp(last().payload, "ON") == 0);
    expect("a command is the bridge's", !rx("zhac/devices/00158D0007A1B2C3/state/set", "ON"));
    expect("own device state: dropped", !rx("zhac/devices/00158D0007A1B2C3/state"));
    expect("own log: dropped", !rx("zhac/log/info", "{\"msg\":\"hi\"}"));
    expect("own metrics: dropped", !rx("zhac/bridge/metrics"));
    expect("own availability: dropped", !rx("zhac/availability", "online"));
    expect("a topic that only starts like ours: kept", rx("zhac/availability_lamp") && rx("zhac/logger"));
    expect("outside the root: kept", rx("other/log/info"));

    const std::string json = R"({"temperature":21.5,"humidity":48.2,"battery":90})";
    char whole[kMqttPayloadMax];
    expect("a long payload: kept", rx("zhac/sensor", json));
    expect("  with the id of its whole text",
           mqtt_msg_full_payload(last(), whole, sizeof(whole)) == json.size() && std::string(whole, json.size()) == json);

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
