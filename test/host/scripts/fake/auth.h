// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_http_server.h"
esp_err_t auth_register_uri(httpd_handle_t hd, const httpd_uri_t* u);
