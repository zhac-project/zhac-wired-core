// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "radio_state.h"

#include "device_backend.h"

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
