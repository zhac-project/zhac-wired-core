// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hub's own (PROTO_LOCAL) devices -- Hub I/O, Alarm and Hub Sound. device_cmd and
// zhc_adapter take one hook each; this routes them by ieee.
#pragma once

#include <cstdint>

#include "device_cmd.h"
#include "zhc_adapter.h"

// At early boot, before anything can send a command.
void local_dev_register(uint64_t ieee, device_cmd_local_fn cmd, zhac_local_exposes_fn exposes);
// Is this one of the hub's own devices (no radio behind it)?
bool local_dev_is(uint64_t ieee);
