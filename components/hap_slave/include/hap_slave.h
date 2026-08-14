// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drop-in replacement for zhac-main-core/components/hap_slave.
// Same symbols + signatures; routes through mono_bridge.
#pragma once
#include "hap_protocol.h"
#include <functional>

using HapFrameCallback = std::function<void(const HapFrame&)>;

void hap_slave_init();
void hap_slave_send(const HapFrame& frame);
void hap_slave_set_callback(HapFrameCallback cb);
