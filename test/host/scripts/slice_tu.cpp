// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL script ops cut out of zhac-wired-core/main/ws_bridge.cpp by run.sh (ws_slice.inc: send_err,
// sha256_hex, reply_ok_or_err, the whole script.* section, ws_push_script_saved, on_script_error), with their
// neighbours stubbed (stubs.h, harness.cpp). The harness calls the static originals through the h_* wrappers.
#include "stubs.h"
#include "json_buf.h"    // the real JsonWriter (zhac-wired-core/main)
#include "ws_bridge.h"   // the real header: ws_push_script_saved, ws_push

// ---- test doubles for the file's push plumbing (the real ones need the WebSocket server and the relay) ----
bool g_listeners = true;
struct Pushed { std::string event, payload; };
std::vector<Pushed> g_events;
std::string g_order;   // 'R' = a reply, 'E' = an event, in the order they happened

static bool push_listeners() { return g_listeners; }
static void push_event(const char* event, const char* payload, size_t len) {
    g_events.push_back({event, std::string(payload, len)});
    g_order += 'E';
}
void ws_push(const char* event, JsonDocument& data) {
    if (!push_listeners()) return;
    std::string s;
    serializeJson(data, s);
    g_events.push_back({event, s});
    g_order += 'E';
}

#include "ws_slice.inc"

void h_send_err(int fd, uint32_t id, const char* err) { send_err(fd, id, err); }
bool h_sha256_hex(const void* data, size_t len, char out[65]) { return sha256_hex(data, len, out); }
void h_list(int fd, uint32_t id) { cmd_script_list(fd, id); }
void h_read(int fd, uint32_t id, JsonDocument& doc) { cmd_script_read(fd, id, doc); }
void h_delete(int fd, uint32_t id, JsonDocument& doc) { cmd_script_delete(fd, id, doc); }
void h_run(int fd, uint32_t id, JsonDocument& doc) { cmd_script_run(fd, id, doc); }
void h_part(int fd, uint32_t id, JsonDocument& doc) { cmd_script_part(fd, id, doc); }
void h_write(int fd, uint32_t id, JsonDocument& doc) { cmd_script_write(fd, id, doc); }
void h_check(int fd, uint32_t id, JsonDocument& doc) { cmd_script_check(fd, id, doc); }
void h_reload(int fd, uint32_t id) { cmd_script_reload(fd, id); }
void h_script_error(const char* name, int line, const char* message) { on_script_error(name, line, message); }
uint32_t h_now_ms() { return now_ms(); }
