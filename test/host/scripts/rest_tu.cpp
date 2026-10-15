// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL api_scripts.cpp (copied verbatim by run.sh as api_scripts_real.cpp) against a fake esp_http_server
// (fake/): a request is a method, a uri, a body and a content length; the response is a status, a type and a body.
#include "stubs.h"
#include "rest.h"
#include "api_scripts_real.cpp"

static std::string g_body;
static size_t g_pos = 0;
static RestWire g_wire;
static RestResult g_res;

// 0 once the body is used up (the peer closed), like the real one
int httpd_req_recv(httpd_req_t*, char* buf, size_t len) {
    if (g_wire.cut_after >= 0 && g_pos >= static_cast<size_t>(g_wire.cut_after)) return g_wire.cut_code;
    size_t n = std::min({len, g_body.size() - g_pos, g_wire.chunk});
    if (g_wire.cut_after >= 0) n = std::min(n, static_cast<size_t>(g_wire.cut_after) - g_pos);
    std::memcpy(buf, g_body.data() + g_pos, n);
    g_pos += n;
    return static_cast<int>(n);
}
size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char* field) {
    return (g_wire.chunked && std::strcmp(field, "Transfer-Encoding") == 0) ? std::strlen("chunked") : 0;
}
esp_err_t httpd_resp_set_type(httpd_req_t*, const char* type) { g_res.type = type; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t*, const char* buf, ssize_t len) {
    g_res.status = 200;
    g_res.body.assign(buf, len < 0 ? std::strlen(buf) : static_cast<size_t>(len));
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* s) { return httpd_resp_send(r, s, -1); }
esp_err_t httpd_resp_send_err(httpd_req_t*, httpd_err_code_t code, const char* msg) {
    g_res.status = static_cast<int>(code);
    g_res.body = msg;
    return ESP_OK;
}
esp_err_t auth_register_uri(httpd_handle_t, const httpd_uri_t*) { return ESP_OK; }

RestResult rest_call(int method, const std::string& uri, const std::string& body, long content_len, RestWire wire) {
    httpd_req_t req{};
    req.method = method;
    std::snprintf(req.uri, sizeof(req.uri), "%s", uri.c_str());
    req.content_len = content_len < 0 ? body.size() : static_cast<size_t>(content_len);
    g_body = body; g_pos = 0; g_wire = wire; g_res = RestResult{};
    if (uri == "/api/scripts") handle_get_scripts(&req);
    else handle_scripts_item(&req);
    return g_res;
}
