// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_rules.cpp — rule REST surface.
//
//   GET    /api/rules                  — list (id, name, dsl, enabled, trigger_type)
//   POST   /api/rules                  — create {"name","dsl"} → returns {"id":N}
//   PUT    /api/rules/<id>             — update {"name","dsl"}
//   DELETE /api/rules/<id>             — delete rule
//   POST   /api/rules/<id>/enable      — toggle {"enabled":bool}
//
// Direct in-process calls into simple_rules + rule_store. No HAP.
// On parse error, the response includes the dsl_parser-formatted
// message in `err` so the SPA can show what the user mistyped.
#include "api_rules.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ArduinoJson.h"
#include "simple_rules.h"
#include "rule_store.h"
#include "zap_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "api_rules";

// Pull a numeric id out of "/api/rules/<id>" or "/api/rules/<id>/enable".
// Returns 0 on parse failure.
static uint16_t parse_id_from_uri(const char* uri, const char* prefix) {
    size_t pl = std::strlen(prefix);
    if (std::strncmp(uri, prefix, pl) != 0) return 0;
    return (uint16_t)strtoul(uri + pl, nullptr, 10);
}

static bool uri_ends_with(const char* uri, const char* suffix) {
    size_t ul = std::strlen(uri), sl = std::strlen(suffix);
    return ul >= sl && std::strcmp(uri + ul - sl, suffix) == 0;
}

// ── GET /api/rules — chunked list ───────────────────────────────────────
static esp_err_t handle_get_rules(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    RuleSlot slots[ZAP_MAX_RULES];
    uint16_t cnt = rule_store_load_all(slots, ZAP_MAX_RULES);
    for (uint16_t i = 0; i < cnt; i++) {
        const RuleSlot& s = slots[i];
        JsonDocument doc;
        doc["id"]           = s.rule_id;
        doc["enabled"]      = (bool)s.enabled;
        doc["trigger_type"] = s.trigger_type;
        doc["rule_type"]    = s.rule_type;
        doc["name"]         = s.name;
        // src is bounded by src_len, not always NUL-terminated.
        char dsl[501];
        size_t n = s.src_len < sizeof(dsl) - 1 ? s.src_len : sizeof(dsl) - 1;
        std::memcpy(dsl, s.src, n);
        dsl[n] = '\0';
        doc["dsl"] = dsl;

        char buf[768];
        size_t bn = serializeJson(doc, buf, sizeof(buf));
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        httpd_resp_send_chunk(req, buf, bn);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

// ── POST /api/rules — create ────────────────────────────────────────────
static esp_err_t handle_post_rules(httpd_req_t* req) {
    char body[640];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char* name = doc["name"] | "";
    const char* dsl  = doc["dsl"]  | (const char*)nullptr;
    if (!dsl || dsl[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dsl");
        return ESP_FAIL;
    }
    uint16_t new_id = 0;
    bool ok = simple_rules_add(name, dsl, &new_id);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        char r[64];
        int rl = snprintf(r, sizeof(r), "{\"ok\":true,\"id\":%u}", (unsigned)new_id);
        return httpd_resp_send(req, r, rl);
    }
    char r[160];
    int rl = snprintf(r, sizeof(r),
                      "{\"ok\":false,\"err\":\"%s\"}",
                      dsl_last_error());
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, r, rl);
}

// ── /api/rules/<id> handlers (PUT/DELETE) and /api/rules/<id>/enable ───
static esp_err_t handle_rules_item(httpd_req_t* req) {
    const char* uri = req->uri;
    uint16_t id = parse_id_from_uri(uri, "/api/rules/");
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_FAIL;
    }

    // /api/rules/<id>/enable  → POST {"enabled":bool}
    if (req->method == HTTP_POST && uri_ends_with(uri, "/enable")) {
        char body[64];
        int n = httpd_req_recv(req, body, sizeof(body) - 1);
        if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
        body[n] = '\0';
        JsonDocument doc;
        if (deserializeJson(doc, body, n)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL;
        }
        bool en = doc["enabled"] | false;
        bool ok = simple_rules_enable(id, en);
        httpd_resp_set_type(req, "application/json");
        return ok
            ? httpd_resp_sendstr(req, "{\"ok\":true}")
            : (httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "rule not found"), ESP_FAIL);
    }

    // PUT /api/rules/<id>
    if (req->method == HTTP_PUT) {
        char body[640];
        int n = httpd_req_recv(req, body, sizeof(body) - 1);
        if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
        body[n] = '\0';
        JsonDocument doc;
        if (deserializeJson(doc, body, n)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL;
        }
        const char* name = doc["name"] | "";
        const char* dsl  = doc["dsl"]  | (const char*)nullptr;
        if (!dsl) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dsl"); return ESP_FAIL; }
        bool ok = simple_rules_update(id, name, dsl);
        httpd_resp_set_type(req, "application/json");
        if (ok) return httpd_resp_sendstr(req, "{\"ok\":true}");
        char r[160];
        int rl = snprintf(r, sizeof(r),
                          "{\"ok\":false,\"err\":\"%s\"}",
                          dsl_last_error());
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, r, rl);
    }

    // DELETE /api/rules/<id>
    if (req->method == HTTP_DELETE) {
        bool ok = simple_rules_delete(id);
        httpd_resp_set_type(req, "application/json");
        return ok
            ? httpd_resp_sendstr(req, "{\"ok\":true}")
            : (httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "rule not found"), ESP_FAIL);
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "use PUT/DELETE");
    return ESP_FAIL;
}

bool api_rules_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};

    u.uri = "/api/rules"; u.method = HTTP_GET;  u.handler = handle_get_rules;
    httpd_register_uri_handler(hd, &u);
    u.uri = "/api/rules"; u.method = HTTP_POST; u.handler = handle_post_rules;
    httpd_register_uri_handler(hd, &u);

    // Wildcard. Specific suffix /enable is detected inside the handler.
    u.uri = "/api/rules/*"; u.handler = handle_rules_item;
    u.method = HTTP_PUT;    httpd_register_uri_handler(hd, &u);
    u.method = HTTP_DELETE; httpd_register_uri_handler(hd, &u);
    u.method = HTTP_POST;   httpd_register_uri_handler(hd, &u);

    ESP_LOGI(TAG, "/api/rules CRUD + /enable registered");
    return true;
}
