// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Sent with every answer of the hub's web server (review HS-14): no MIME sniffing, and no Referer, so a
// link out of the hub page does not tell the other site the hub's LAN address. Each door sets them once
// (auth's gate, login and set-up, /api/status, the web UI files).
#pragma once

#include "esp_http_server.h"

inline void sec_headers(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}
