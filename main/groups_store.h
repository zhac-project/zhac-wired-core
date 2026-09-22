// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
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

// Create the store mutex once, from the single-task boot context, before the
// httpd / remote server tasks start. Avoids two concurrent first-requests each
// lazily creating (and leaking) a mutex. Idempotent.
void grp_store_init();

// Returns the next available group ID (1-based), or 0 if all slots are full.
uint16_t grp_next_id();

// Load all saved groups into `out` (max `max`). Returns count loaded.
uint16_t grp_load_all(GrpRecord* out, uint16_t max);

// Save a group record to NVS. Returns true on success.
bool grp_save(const GrpRecord& r);

// Atomically allocate the next free id INTO `r.id` and persist `r`. Use this
// instead of grp_next_id()+grp_save() from request handlers: it holds the
// store lock across both steps so two concurrent creators (httpd vs the
// remote-allow-listed cloud task) cannot grab the same slot. Returns false if
// the table is full or the save fails. On success `r.id` is the new id.
bool grp_create(GrpRecord& r);

// Delete group by id. Returns true on success.
bool grp_delete(uint16_t id);

// Find group by id, filling `out`. Returns true if found.
bool grp_find(uint16_t id, GrpRecord& out);

// Serialise one GrpRecord to JSON in buf[cap]. Returns length, or 0 on overflow.
size_t grp_to_json(const GrpRecord& r, char* buf, size_t cap);

// Store-lock accessors (recursive). Use ONLY to bracket a load-into-shared-
// buffer + read-back sequence (see api_group_list) so the buffer cannot be
// refilled by a concurrent caller on another task mid-read. The individual
// grp_* ops already lock internally; do not wrap single calls.
void grp_store_lock();
void grp_store_unlock();
