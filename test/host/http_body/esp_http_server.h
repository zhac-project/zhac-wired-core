// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-in: a request whose body arrives in pieces (harness.cpp).
#pragma once
#include <cstddef>
typedef struct httpd_req { size_t content_len; } httpd_req_t;
#define HTTPD_SOCK_ERR_TIMEOUT -3
int httpd_req_recv(httpd_req_t* r, char* buf, size_t len);
