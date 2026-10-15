// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REST side of the harness: rest_tu.cpp runs the real api_scripts.cpp handlers against a fake httpd.
#pragma once
#include <string>
struct RestResult { int status = 0; std::string type, body; };
// What the fake socket does with the body. `chunk`: at most this many bytes per httpd_req_recv. `cut_after` (>= 0):
// once that many bytes were delivered, every receive answers `cut_code` instead: 0 = the client closed the
// connection, -3 = HTTPD_SOCK_ERR_TIMEOUT, -1 = HTTPD_SOCK_ERR_FAIL, -2 = HTTPD_SOCK_ERR_INVALID.
struct RestWire {
    size_t chunk = static_cast<size_t>(-1);
    long   cut_after = -1;
    int    cut_code = 0;
    bool   chunked = false;   // a Transfer-Encoding: chunked request (esp_http_server then reports content_len 0)
};
// content_len < 0: the body's own length
RestResult rest_call(int method, const std::string& uri, const std::string& body, long content_len = -1,
                     RestWire wire = RestWire());
