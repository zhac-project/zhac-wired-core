// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth.h"
#include "api_groups.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ArduinoJson.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "device_cmd.h"
#include "device_cmd_json.h"
#include "groups_store.h"
#include "json_buf.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"

static const char* TAG = "api_groups";

// Local ieee parser (ws_bridge's is file-static). Accepts optional "0x".
static uint64_t grp_parse_ieee(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return strtoull(s, nullptr, 16);
}

static void parse_members(const char* body, size_t len, GrpRecord& r) {
    r.member_count = 0;
    JsonDocument doc;
    if (deserializeJson(doc, body, len)) return;
    for (JsonObject obj : doc["members"].as<JsonArray>()) {
        if (r.member_count >= GRP_MAX_MEMBERS) break;
        const char* ieee_str = obj["ieee"] | (const char*)nullptr;
        if (!ieee_str) continue;
        r.members[r.member_count++] = { grp_parse_ieee(ieee_str),
                                        (uint8_t)(obj["ep"] | 1) };
    }
}

// ── shared logic ────────────────────────────────────────────────────────────

// 0 when the list does not fit `cap` (GRP_LIST_JSON_MAX always does): a
// caller must never send a partial or overrun list (review F10).
size_t group_list_json(char* out, size_t cap) {
    static GrpRecord all[GRP_MAX_GROUPS];
    static char row[GRP_JSON_MAX];
    // Shared static buffers: hold the store lock across load + serialise so a
    // concurrent group.list on another task cannot refill them mid-read.
    grp_store_lock();
    const uint16_t cnt = grp_load_all(all, GRP_MAX_GROUPS);
    JsonWriter w(out, cap);
    w.raw("{\"groups\":[");
    bool first = true;
    for (uint16_t i = 0; i < cnt; i++) {
        const size_t n = grp_to_json(all[i], row, sizeof(row));
        if (!n) continue;                  // cannot happen at GRP_JSON_MAX; never a bare comma
        if (!first) w.ch(',');
        w.raw(row, n);
        first = false;
    }
    w.raw("]}");
    const size_t len = w.finish();
    grp_store_unlock();
    if (len < cap) out[len] = '\0';
    return len;
}

bool group_create(const char* body, size_t len, char* out, size_t cap, size_t* n) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    GrpRecord r{};
    strncpy(r.name, doc["name"] | "", sizeof(r.name) - 1);
    parse_members(body, len, r);
    if (!grp_create(r)) return false;   // id allocation + save under one lock
    const size_t w = grp_to_json(r, out, cap);
    if (n) *n = w;
    return w > 0;                       // out holds GRP_JSON_MAX: always fits
}

bool group_get(const char* body, size_t len, char* out, size_t cap, size_t* n) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    GrpRecord r{};
    if (!grp_find(doc["id"] | (uint16_t)0, r)) return false;
    const size_t w = grp_to_json(r, out, cap);
    if (n) *n = w;
    return w > 0;
}

bool group_update(const char* body, size_t len, char* out, size_t cap, size_t* n) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    GrpRecord r{};
    if (!grp_find(doc["id"] | (uint16_t)0, r)) return false;
    if (!doc["name"].isNull())    strncpy(r.name, doc["name"] | "", sizeof(r.name) - 1);
    if (!doc["members"].isNull()) parse_members(body, len, r);
    if (!grp_save(r)) return false;
    const size_t w = grp_to_json(r, out, cap);
    if (n) *n = w;
    return w > 0;
}

bool group_delete_req(const char* body, size_t len) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    return grp_delete(doc["id"] | (uint16_t)0);
}

size_t group_cmd(const char* body, size_t len, char* out, size_t cap) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len))
        return snprintf(out, cap, "{\"ok\":false,\"err\":\"bad json\"}");
    GrpRecord r{};
    if (!grp_find(doc["id"] | (uint16_t)0, r) || r.member_count == 0)
        return snprintf(out, cap, "{\"ok\":false,\"err\":\"not found\"}");

    // The web UI's buttons send a ZCL command, {cluster, cmd}: On / Off /
    // Toggle / Identify, as on the dual-chip hub (net-core api_group_cmd).
    // Other callers send {key, val}. Both become the members' typed attribute
    // set. A request with neither, or an unusable value, is refused: it used to
    // become state = 0, so every button -- On included -- turned the group off
    // (review F34).
    char key[24] = "state";
    DevCmdValue val;
    if (!doc["cmd"].isNull()) {
        const uint16_t cluster = doc["cluster"] | (uint16_t)0;
        const int      cmd     = doc["cmd"].is<int>() ? doc["cmd"].as<int>() : -1;
        if (cluster == 0x0006 && cmd >= 0 && cmd <= 2) {
            val = device_cmd_str(cmd == 1 ? "ON" : cmd == 0 ? "OFF" : "TOGGLE");
        } else if (cluster == 0x0003 && cmd == 0) {
            strcpy(key, "identify");
            val = device_cmd_str("identify");
        } else {
            return snprintf(out, cap, "{\"ok\":false,\"err\":\"unsupported group command\"}");
        }
    } else {
        strncpy(key, doc["key"] | "state", sizeof(key) - 1);
        if (!device_cmd_value_from_json(doc["val"], &val))
            return snprintf(out, cap, "{\"ok\":false,\"err\":\"val must be a boolean, number or string\"}");
    }

    uint8_t sent = 0, failed = 0;
    for (uint8_t i = 0; i < r.member_count; i++) {
        if (device_cmd_set_attr(r.members[i].ieee, r.members[i].ep, key, &val) == DEVCMD_OK)
            sent++;
        else
            failed++;
    }
    bool ok = (sent == r.member_count);
    return snprintf(out, cap, "{\"ok\":%s,\"sent\":%u,\"failed\":%u}",
                    ok ? "true" : "false", sent, failed);
}

// ── REST wrappers ───────────────────────────────────────────────────────────

static int recv_body(httpd_req_t* req, char* buf, size_t cap) {
    int n = httpd_req_recv(req, buf, cap - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

static esp_err_t h_group_list(httpd_req_t* req) {
    char* buf = static_cast<char*>(heap_caps_malloc(GRP_LIST_JSON_MAX, MALLOC_CAP_SPIRAM));
    const size_t n = buf ? group_list_json(buf, GRP_LIST_JSON_MAX) : 0;
    if (!n) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "group list unavailable");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t e = httpd_resp_send(req, buf, n);
    heap_caps_free(buf);
    return e;
}

static esp_err_t h_group_create(httpd_req_t* req) {
    char buf[1024];
    int n = recv_body(req, buf, sizeof(buf));
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    char rsp[GRP_JSON_MAX]; size_t rn = 0;
    httpd_resp_set_type(req, "application/json");
    if (!group_create(buf, n, rsp, sizeof(rsp), &rn)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "create failed");
        return ESP_FAIL;
    }
    return httpd_resp_send(req, rsp, rn);
}

static esp_err_t h_group_get(httpd_req_t* req) {
    char buf[256];
    int n = recv_body(req, buf, sizeof(buf));
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    char rsp[GRP_JSON_MAX]; size_t rn = 0;
    httpd_resp_set_type(req, "application/json");
    if (!group_get(buf, n, rsp, sizeof(rsp), &rn)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such group");
        return ESP_FAIL;
    }
    return httpd_resp_send(req, rsp, rn);
}

static esp_err_t h_group_update(httpd_req_t* req) {
    char buf[1024];
    int n = recv_body(req, buf, sizeof(buf));
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    char rsp[GRP_JSON_MAX]; size_t rn = 0;
    httpd_resp_set_type(req, "application/json");
    if (!group_update(buf, n, rsp, sizeof(rsp), &rn)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such group");
        return ESP_FAIL;
    }
    return httpd_resp_send(req, rsp, rn);
}

static esp_err_t h_group_delete(httpd_req_t* req) {
    char buf[256];
    int n = recv_body(req, buf, sizeof(buf));
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, group_delete_req(buf, n) ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t h_group_cmd(httpd_req_t* req) {
    char buf[512];
    int n = recv_body(req, buf, sizeof(buf));
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    char rsp[128];
    size_t rn = group_cmd(buf, n, rsp, sizeof(rsp));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, rsp, rn);
}

bool api_groups_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};
    struct { const char* uri; httpd_method_t m; esp_err_t (*h)(httpd_req_t*); } routes[] = {
        { "/api/groups",       HTTP_GET,  h_group_list   },
        { "/api/groups",       HTTP_POST, h_group_create },
        { "/api/group/get",    HTTP_POST, h_group_get    },
        { "/api/group/update", HTTP_POST, h_group_update },
        { "/api/group/delete", HTTP_POST, h_group_delete },
        { "/api/group/cmd",    HTTP_POST, h_group_cmd    },
    };
    for (auto& r : routes) {
        u.uri = r.uri; u.method = r.m; u.handler = r.h;
        auth_register_uri(hd, &u);
    }
    ESP_LOGI(TAG, "group routes registered");
    return true;
}
