// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A REST body is applied only once all of it has arrived (review F29):
// httpd_req_recv() returns what is there, not content_len.
#include "http_body.h"

#include <cstdio>
#include <cstring>
#include <string>

static std::string g_body;        // what the client sends
static size_t      g_pos = 0;
static size_t      g_piece = 1;   // bytes per recv
static int         g_timeouts = 0, g_eof_at = -1;
int httpd_req_recv(httpd_req_t*, char* buf, size_t len) {
    if (g_timeouts > 0) { g_timeouts--; return HTTPD_SOCK_ERR_TIMEOUT; }
    if (g_eof_at >= 0 && g_pos >= (size_t)g_eof_at) return 0;
    size_t n = g_body.size() - g_pos;
    if (n > g_piece) n = g_piece;
    if (n > len) n = len;
    std::memcpy(buf, g_body.data() + g_pos, n);
    g_pos += n;
    return (int)n;
}

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static int read(const std::string& body, size_t declared, size_t piece, char* buf, size_t cap,
                int timeouts = 0, int eof_at = -1) {
    g_body = body; g_pos = 0; g_piece = piece; g_timeouts = timeouts; g_eof_at = eof_at;
    httpd_req_t req{declared};
    return http_read_body(&req, buf, cap);
}

int main() {
    char buf[64];
    const std::string json = "{\"name\":\"hall\",\"members\":[]}";
    int n = read(json, json.size(), 1, buf, sizeof(buf));
    expect("a body in one-byte pieces arrives whole", n == (int)json.size() && json == buf);
    n = read(json, json.size(), 7, buf, sizeof(buf), 2);
    expect("  also across a couple of slow reads", n == (int)json.size() && json == buf);
    expect("a body larger than the buffer is refused before reading",
           read(std::string(64, 'x'), 64, 64, buf, sizeof(buf)) == -1 && g_pos == 0);
    expect("a client that stops early is refused", read(json, json.size(), 4, buf, sizeof(buf), 0, 10) == -1);
    expect("a client that never sends is refused", read(json, json.size(), 4, buf, sizeof(buf), 100) == -1);
    expect("no body is refused", read("", 0, 1, buf, sizeof(buf)) == -1);
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nrequest bodies OK\n", g_fail);
    return g_fail;
}
