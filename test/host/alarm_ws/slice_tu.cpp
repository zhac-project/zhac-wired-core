// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL alarm.* commands and the replies they use, cut out of main/ws_bridge.cpp by run.sh (ws_slice.inc). The
// harness calls them through the h_* wrappers below, because the originals are static.
#include <cinttypes>
#include <cstdio>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"   // the stub here: counted, can fail
#include "json_buf.h"        // the real JsonWriter (main/)
#include "psram_json.h"      // the real PSRAM allocator (main/), on the stub above
#include "alarm.h"           // the real AlarmReply and the panel's calls (main/); harness.cpp fakes the calls

void ws_server_reply(int fd, const char* buf, size_t len);   // harness.cpp: every frame sent
struct ZapDevice { uint64_t ieee_addr; char friendly_name[33]; };
bool zigbee_pool_snapshot(uint64_t ieee, ZapDevice* out);    // harness.cpp: no device names

#include "ws_slice.inc"

void h_alarm_get(int fd, uint32_t id) { cmd_alarm_get(fd, id); }
void h_alarm_arm(int fd, uint32_t id, JsonDocument& doc) { cmd_alarm_arm(fd, id, doc); }
void h_alarm_disarm(int fd, uint32_t id, JsonDocument& doc) { cmd_alarm_disarm(fd, id, doc); }
void h_alarm_silence(int fd, uint32_t id, JsonDocument& doc) { cmd_alarm_silence(fd, id, doc); }
