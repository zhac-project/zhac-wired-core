// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Read a REST request's whole body. httpd_req_recv() returns what has arrived,
// not necessarily content_len, so a body split across TCP segments was parsed
// from its first piece: valid requests failed, and a complete-looking JSON
// prefix could be applied (review F29). Host check: test/host/http_body/.
#pragma once

#include <cstddef>

#include "esp_http_server.h"

// The body, NUL-terminated in `buf`, and its length; -1 when there is none,
// it does not fit `cap` (checked before reading anything), or the client stops
// sending (closes, or times out three times running).
inline int http_read_body(httpd_req_t* req, char* buf, size_t cap) {
    if (req->content_len == 0 || req->content_len >= cap) return -1;
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (r <= 0) return -1;
        timeouts = 0;
        got += static_cast<size_t>(r);
    }
    buf[got] = '\0';
    return static_cast<int>(got);
}
