// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The early-crash guard's verdict (ota_update.cpp). Pure, host-checked in test/host/boot_guard.
//
// Boots that end in a crash within two minutes of starting are counted in a row. The radio has its own guard
// (three trips, then the hub runs without it) and a Lua script that crashes the hub is held, so the first answer
// is safe mode: no Lua, MQTT or cloud link. Only an image that keeps crashing in safe mode, and has not yet run a
// day, goes back to the other slot (research 2026-11-18 A1): an older build rarely fixes a fault that appears in
// a day-old one, and a rollback can lose what the device store saved since.
#pragma once
#include <cstdint>

namespace boot_guard {

constexpr uint32_t kSafeModeAt  = 3;    // early crashes in a row
constexpr uint32_t kRollbackAt  = 5;    // so two safe-mode boots crashed as well
constexpr uint8_t  kProvenHours = 24;   // run time after which an image is never rolled back

enum class Verdict : uint8_t { Run, SafeMode, Rollback };

constexpr Verdict verdict(uint32_t early_crashes, uint8_t image_hours, bool other_slot_bootable) {
    if (early_crashes < kSafeModeAt) return Verdict::Run;
    if (early_crashes >= kRollbackAt && image_hours < kProvenHours && other_slot_bootable) return Verdict::Rollback;
    return Verdict::SafeMode;
}

// The stored count belongs to the image whose id (first 4 bytes of its ELF SHA-256) is stored with it.
constexpr uint8_t image_hours(uint32_t stored_id, uint8_t stored_hours, uint32_t running_id) {
    return stored_id == running_id ? stored_hours : 0;
}

constexpr uint8_t next_hour(uint8_t h) { return h < kProvenHours ? h + 1 : h; }

}  // namespace boot_guard
