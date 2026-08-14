// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "api_groups.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "groups_store.h"
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

size_t group_list_json(char* out, size_t cap) {
    static GrpRecord all[GRP_MAX_GROUPS];
    uint16_t cnt = grp_load_all(all, GRP_MAX_GROUPS);
    size_t pos = 0;
    pos += snprintf(out + pos, cap - pos, "{\"groups\":[");
    for (uint16_t i = 0; i < cnt && pos < cap; i++) {
        if (i) out[pos++] = ',';
        char tmp[256];
        size_t n = grp_to_json(all[i], tmp, sizeof(tmp));
        if (pos + n < cap) { memcpy(out + pos, tmp, n); pos += n; }
    }
    pos += snprintf(out + pos, cap - pos, "]}");
    return pos;
}

bool group_create(const char* body, size_t len, char* out, size_t cap, size_t* n) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    GrpRecord r{};
    r.id = grp_next_id();
    strncpy(r.name, doc["name"] | "", sizeof(r.name) - 1);
    parse_members(body, len, r);
    if (r.id == 0 || !grp_save(r)) return false;
    size_t w = grp_to_json(r, out, cap);
    if (n) *n = w;
    return true;
}

bool group_get(const char* body, size_t len, char* out, size_t cap, size_t* n) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    GrpRecord r{};
    if (!grp_find(doc["id"] | (uint16_t)0, r)) return false;
    size_t w = grp_to_json(r, out, cap);
    if (n) *n = w;
    return true;
}

bool group_update(const char* body, size_t len, char* out, size_t cap, size_t* n) {
    JsonDocument doc;
    if (len == 0 || deserializeJson(doc, body, len)) return false;
    GrpRecord r{};
    if (!grp_find(doc["id"] | (uint16_t)0, r)) return false;
    if (!doc["name"].isNull())    strncpy(r.name, doc["name"] | "", sizeof(r.name) - 1);
    if (!doc["members"].isNull()) parse_members(body, len, r);
    if (!grp_save(r)) return false;
    size_t w = grp_to_json(r, out, cap);
    if (n) *n = w;
    return true;
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

    char key[24] = "state";
    strncpy(key, doc["key"] | "state", sizeof(key) - 1);
    uint64_t val = doc["val"] | (uint64_t)0;

    uint8_t sent = 0, failed = 0;
    for (uint8_t i = 0; i < r.member_count; i++) {
        zigbee_pool_lock();
        ZapDevice* dev = pool_find_by_ieee(r.members[i].ieee);
        if (!dev) { zigbee_pool_unlock(); failed++; continue; }
        const uint64_t ieee_cp = dev->ieee_addr;
        const uint16_t nwk_cp  = dev->nwk_addr;
        const uint8_t  ep_cp   = r.members[i].ep ? r.members[i].ep
                                 : (dev->endpoints[0] ? dev->endpoints[0] : 1);
        char model_cp[64], manu_cp[64];
        snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
        snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
        zigbee_pool_unlock();

        if (zhac_adapter_send_uint(ieee_cp, model_cp, manu_cp, nwk_cp, ep_cp, key, val))
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
    char buf[4096];
    size_t n = group_list_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_group_create(httpd_req_t* req) {
    char buf[1024];
    int n = recv_body(req, buf, sizeof(buf));
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    char rsp[512]; size_t rn = 0;
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
    char rsp[512]; size_t rn = 0;
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
    char rsp[512]; size_t rn = 0;
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
        httpd_register_uri_handler(hd, &u);
    }
    ESP_LOGI(TAG, "group routes registered");
    return true;
}
