// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "radio_state.h"

#include "device_backend.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "zigbee_mgr.h"   // zigbee_permit_join

namespace {
// Single source of truth for the join window, shared by the REST handler and
// the WS verb. Written only from httpd/WS handler context.
int64_t s_permit_deadline_us = 0;
uint8_t s_permit_duration_s  = 0;
}  // namespace

bool radio_present() {
    return device_backend_count() > 0;
}

bool radio_ok() {
    const uint8_t n = device_backend_count();
    for (uint8_t i = 0; i < n; i++) {
        DeviceBackend* b = device_backend_get(i);
        // is_running is a function pointer in the DeviceBackend vtable and a
        // backend is free to leave it null; treat that as "registered but
        // cannot answer", which is not the same as running.
        if (b && b->is_running && b->is_running()) return true;
    }
    return false;
}

bool radio_permit_join(uint8_t duration_s) {
    if (!zigbee_permit_join(duration_s)) {
        // Leave the previous deadline untouched: a failed open must not report
        // the network as closed if an earlier window is genuinely still open.
        return false;
    }
    if (duration_s > 0) {
        s_permit_deadline_us =
            esp_timer_get_time() + static_cast<int64_t>(duration_s) * 1000000LL;
        s_permit_duration_s = duration_s;
    } else {
        s_permit_deadline_us = 0;
        s_permit_duration_s  = 0;
    }
    ESP_LOGI("radio_state", "permit_join %us", (unsigned)duration_s);
    return true;
}

void radio_permit_join_status(bool* open_out, int* remaining_s_out) {
    const int64_t now = esp_timer_get_time();
    const bool open = s_permit_deadline_us > now;
    // Round up so a window with 100 ms left still reads as 1 s rather than 0 --
    // the SPA hides its countdown badge at zero, and flashing "closed" while
    // the radio is still accepting joins is worse than a rounding error.
    const int remaining =
        open ? static_cast<int>((s_permit_deadline_us - now + 999999LL) / 1000000LL) : 0;
    if (open_out)        *open_out = open;
    if (remaining_s_out) *remaining_s_out = remaining;
}
