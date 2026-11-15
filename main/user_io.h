// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hub pins (spec extra/docs/superpowers/specs/2026-11-15-gpio-control-design.md): the configured
// pins, their hardware, and the "Hub I/O" device that shows them to the hub page, the cloud and HA.
// Rules, Lua and device_cmd all come through here; only configured pins are ever touched.
#pragma once

#include <cstddef>
#include <cstdint>

#include "user_io_config.h"

// The Hub I/O device: locally administered IEEE ("ZHACIO"), protocol PROTO_LOCAL.
constexpr uint64_t kHubIoIeee = 0x025A484143494F00ULL;

// Boot, in this order: load the config, set the pins, register the hooks
// (before simple_rules_init and lua_engine_init) ...
void user_io_early_init();
// ... add or drop the Hub I/O pool entry (after the pool restore) ...
void user_io_attach_device();
// ... publish the pin values and start the poll task (end of boot).
void user_io_start();

// PUT /api/io: save, then apply live. False with the reason when it was not saved.
bool user_io_apply(const UserIoConfig& cfg, char* err, size_t cap);

const UserIoCaps& user_io_caps();
bool user_io_adc_in_mv();   // ADC values are mV (calibrated) rather than raw counts

// Config plus runtime state, for GET /api/io.
struct UserIoState {
    UserIoConfig cfg;
    int32_t      value[kUserIoMaxPins];
    char         err[kUserIoMaxPins][40];
};
void user_io_snapshot(UserIoState* out);

// Pin operations shared by rules, Lua and the device handler. `ref` is a pin
// name or its GPIO number; it must be configured in the mode the call needs.
bool user_io_write(const char* ref, UserIoMode want, double v, char* err, size_t cap);   // out: v != 0 = on; pwm: duty %
int  user_io_toggle(const char* ref, char* err, size_t cap);                             // new level, -1 on error
bool user_io_read(const char* ref, int32_t* value, int32_t* raw, char* err, size_t cap); // out/in: level; adc: mV|raw, raw
bool user_io_strip(const char* ref, const uint32_t* rgb, size_t n, char* err, size_t cap); // 0xRRGGBB per LED

// lua_io.cpp: adds zhac.gpio_* & co. to the Lua `zhac` table. Before lua_engine_init.
void lua_io_register();
