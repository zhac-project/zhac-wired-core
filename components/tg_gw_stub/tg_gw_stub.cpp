// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Telegram gateway stubs for targets the sibling tg_gw component does not
// cover (currently esp32s31 -- see this component's CMakeLists for why).
//
// Beyond the missing target, the sibling's implementations are dual-chip by
// design: tg_gw_p4.cpp forwards over HAP to an S3 that runs the actual HTTPS
// client. There is no second chip in this SKU, so even the P4 build's Telegram
// path is inert. If Telegram is ever wanted here it needs a real single-chip
// implementation (esp_http_client + TLS, direct), not a port of either half.
#include "esp_log.h"
#include "tg_gw.h"

static const char* TAG = "tg_gw";

extern "C" {

void tg_gw_init(void) {
    ESP_LOGI(TAG, "Telegram gateway not implemented on this SKU -- inert");
}

bool tg_gw_settoken(const char* /*token*/) {
    ESP_LOGW(TAG, "settoken: Telegram gateway unavailable on this SKU");
    return false;
}

bool tg_gw_setchat(const char* /*chat_id_str*/) {
    ESP_LOGW(TAG, "setchat: Telegram gateway unavailable on this SKU");
    return false;
}

bool tg_gw_send(const char* /*text*/, const char* /*chat_id_or_null*/,
                const char* /*parse_mode_or_null*/) {
    ESP_LOGW(TAG, "send: Telegram gateway unavailable on this SKU");
    return false;
}

}  // extern "C"
