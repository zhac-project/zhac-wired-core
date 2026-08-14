// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drop-in replacement for zhac-net-core/components/hap_master.
// Same symbols + signatures; the implementation routes calls into
// the local in-process services via mono_bridge instead of pushing
// frames over SPI to a separate P4 chip. Consumers compile and link
// unchanged.
#pragma once
#include "hap_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <functional>

using HapFrameCallback = std::function<void(const HapFrame&)>;

void hap_master_init();
void hap_master_set_task_handle(TaskHandle_t h);
void hap_master_send(const HapFrame& frame);
void hap_master_recv();
void hap_master_set_callback(HapFrameCallback cb);
