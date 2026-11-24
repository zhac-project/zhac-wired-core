// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Alarm panel ("security mode", spec extra/docs/superpowers/specs/2026-11-24-security-mode-design.md):
// the glue around alarm_core -- sensors and keypads in, sirens, notifications and the
// "Alarm" device out, the config and state in NVS.
#pragma once

#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

#include "alarm_core.h"

// The Alarm device: locally administered IEEE ("ZHACAL"), protocol PROTO_LOCAL.
constexpr uint64_t kAlarmIeee = 0x025A484143414C00ULL;

// Boot, in this order: load the config and the state, register the device hooks
// (beside user_io_early_init) ...
void alarm_early_init();
// ... add the Alarm pool entry (after the pool restore) ...
void alarm_attach_device();
// ... watch sensors and keypads, publish the state, start the task (before the event pump).
void alarm_start();

// The hub page (WS alarm.*). Arm: OK; OPEN with the open contact sensors in `open`
// (room for kAlarmMaxSensors); BUSY while the alarm is on. Same as setting the
// Alarm device's alarm_state, plus force.
AlarmArmResult alarm_arm(AlarmMode m, bool force, uint64_t* open, uint8_t* n_open);
void alarm_disarm();
// Replace the config ("codes" left out keeps the stored ones). False with the first
// problem in `err` when nothing was changed.
bool alarm_config_set(const char* json, size_t len, char* err, size_t cap);
// {state, mode, since, left?, cause?, sensor?, bypassed[]}: alarm.get and alarm.changed.
void alarm_status_json(JsonObject o);
// The config without the codes ("codes_count").
void alarm_config_json(JsonObject o);
