// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fakes for ws_bridge.cpp's on_device_availability(). See run.sh.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "event_bus.h"

static uint32_t    g_clock = 0;
static std::string g_event, g_payload;
static bool        push_listeners() { return true; }
static uint32_t    esp_zigbee_backend_wall_clock_s() { return g_clock; }
static void push_event(const char* event, const char* payload, size_t len) {
    g_event = event;
    g_payload.assign(payload, len);
}

#include "slice.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static Event ev(EventType type) {
    Event e{};
    e.type = type;
    const uint64_t ieee = 0x00158D0007A1B2C3ULL;
    std::memcpy(e.data, &ieee, sizeof(ieee));
    return e;
}

int main() {
    g_clock = 1763520000;
    on_device_availability(ev(EventType::DEVICE_OFFLINE));
    expect("ping unanswered: offline, no new last_seen",
           g_event == "attr.bulk" && g_payload == R"([{"ieee":"0x00158D0007A1B2C3","offline":true,"attrs":{}}])");
    on_device_availability(ev(EventType::DEVICE_SEEN));
    expect("heard again: online with its new last_seen",
           g_payload == R"([{"ieee":"0x00158D0007A1B2C3","last_seen":1763520000,"offline":false,"attrs":{}}])");
    g_clock = 0;
    g_payload.clear();
    on_device_availability(ev(EventType::DEVICE_SEEN));
    expect("  without a clock: online all the same",
           g_payload == R"([{"ieee":"0x00158D0007A1B2C3","offline":false,"attrs":{}}])");
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
