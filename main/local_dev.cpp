// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "local_dev.h"

namespace {

struct LocalDev {
    uint64_t              ieee;
    device_cmd_local_fn   cmd;
    zhac_local_exposes_fn exposes;
};
LocalDev s_devs[2];   // Hub I/O, Alarm
uint8_t  s_n = 0;

const LocalDev* find(uint64_t ieee) {
    for (uint8_t i = 0; i < s_n; i++) if (s_devs[i].ieee == ieee) return &s_devs[i];
    return nullptr;
}

DevCmdResult route_cmd(uint64_t ieee, const char* key, const DevCmdValue* v) {
    const LocalDev* d = find(ieee);
    return d ? d->cmd(ieee, key, v) : DEVCMD_NOT_FOUND;
}

size_t route_exposes(uint64_t ieee, char* buf, size_t cap) {
    const LocalDev* d = find(ieee);
    return d ? d->exposes(ieee, buf, cap) : 0;
}

}  // namespace

void local_dev_register(uint64_t ieee, device_cmd_local_fn cmd, zhac_local_exposes_fn exposes) {
    if (s_n >= sizeof(s_devs) / sizeof(s_devs[0]) || find(ieee)) return;
    s_devs[s_n++] = {ieee, cmd, exposes};
    device_cmd_set_local_handler(route_cmd);
    zhac_adapter_set_local_exposes_fn(route_exposes);
}

bool local_dev_is(uint64_t ieee) { return find(ieee) != nullptr; }
