// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Copied verbatim from zhac-net-core/main/groups_store.h — NVS-only, no P4
// dependency, so it ports unchanged to the single-chip build.
#pragma once
#include <cstdint>
#include "nvs.h"

// ── Groups NVS storage ────────────────────────────────────────────────────
// Namespace: "zhac_grp"
// Keys: "bmp" (uint32 slot bitmap), "g%04x" (blob of GrpRecord per group)

static constexpr uint8_t GRP_MAX_MEMBERS = 16;
static constexpr uint8_t GRP_MAX_GROUPS  = 32;

struct GrpMember { uint64_t ieee; uint8_t ep; };
struct GrpRecord {
    uint16_t id;
    char     name[32];
    uint8_t  member_count;
    GrpMember members[GRP_MAX_MEMBERS];
};

// Returns the next available group ID (1-based), or 0 if all slots are full.
uint16_t grp_next_id();

// Load all saved groups into `out` (max `max`). Returns count loaded.
uint16_t grp_load_all(GrpRecord* out, uint16_t max);

// Save a group record to NVS. Returns true on success.
bool grp_save(const GrpRecord& r);

// Delete group by id. Returns true on success.
bool grp_delete(uint16_t id);

// Find group by id, filling `out`. Returns true if found.
bool grp_find(uint16_t id, GrpRecord& out);

// Serialise one GrpRecord to JSON in buf[cap]. Returns length, or 0 on overflow.
size_t grp_to_json(const GrpRecord& r, char* buf, size_t cap);
