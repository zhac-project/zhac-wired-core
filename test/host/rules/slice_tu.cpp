// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL error replies, fingerprint helper, rule ops and rule pushes, cut out of main/ws_bridge.cpp by run.sh
// (ws_slice.inc), with their neighbours stubbed (stubs.h). The harness calls them through the h_* wrappers
// below, because the originals are static.
#include "stubs.h"
#include "json_buf.h"       // the real JsonWriter (main/)
#include "remote_xfer.h"    // the real kRemotePageBudget (zhac-net-core remote_client)
#include "event_bus.h"      // the real Event, RuleChangedEvent, RULE_CHANGE_* (zhac-components)

// ---- the file's push plumbing, captured (the real one needs the WebSocket server and the relay) ----
std::vector<Pushed> g_events;
static bool push_listeners() { return true; }
static void push_event(const char* event, const char* payload, size_t len) {
    g_events.push_back({event, std::string(payload, len)});
}
void ws_push(const char* event, JsonDocument& data) {   // ws_bridge.cpp's: serialized into 1,024 bytes
    char buf[1024];
    const size_t n = serializeJson(data, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return;               // dropped, as the real one does
    push_event(event, buf, n);
}

#include "ws_slice.inc"

void h_send_err(int fd, uint32_t id, const char* err) { send_err(fd, id, err); }
void h_reply_ok_or_err(int fd, uint32_t id, bool ok, const char* err) { reply_ok_or_err(fd, id, ok, err); }
bool h_sha256_hex(const void* data, size_t len, char out[65]) { return sha256_hex(data, len, out); }
void h_rule_list(int fd, uint32_t id, JsonDocument& doc) { cmd_rule_list(fd, id, doc); }
void h_rule_create(int fd, uint32_t id, JsonDocument& doc) { cmd_rule_create(fd, id, doc); }
void h_rule_update(int fd, uint32_t id, JsonDocument& doc) { cmd_rule_update(fd, id, doc); }
void h_rule_check(int fd, uint32_t id, JsonDocument& doc) { cmd_rule_check(fd, id, doc); }
size_t h_page_budget() { return kRemotePageBudget; }
void h_rule_changed(uint16_t id, uint8_t change) {
    Event e{};
    e.type = EventType::RULE_CHANGED;
    const RuleChangedEvent c{id, change};
    std::memcpy(e.data, &c, sizeof(c));
    on_rule_changed(e);
}
