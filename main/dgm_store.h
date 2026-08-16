// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PROVENANCE: copied verbatim from zhac-net-core/main/. Both apps need the same
// per-device group mirror and there is no shared component for it yet. Keep the
// two in sync, or promote this to zhac-components -- do NOT let them drift, the
// SPA talks the same protocol to both.
//
// dgm_store — per-device ZCL group-membership MIRROR (increment 2 of the
// native-groups design). ZHAC tracks, per device, the ZCL groups it has
// provisioned the device into (device.groups.add/remove), so the UI can show a
// membership list without querying every device. This is a gateway-side cache;
// the authoritative source is the device's own group table (a later "refresh
// from device" via ZCL Get Group Membership reconciles it via dgm_set).
#pragma once
#include <cstdint>
#include <cstddef>

static constexpr uint8_t DGM_MAX_GIDS = 16;   // groups a single device is tracked in
static constexpr uint8_t DGM_MAX_DEVS = 64;   // devices tracked

struct DgmEntry { uint64_t ieee; uint8_t count; uint16_t gids[DGM_MAX_GIDS]; };
struct DgmTable { uint8_t n; DgmEntry devs[DGM_MAX_DEVS]; };

// ── Pure table operations (no NVS, unit-tested) ──────────────────────────────
// Add gid to ieee's list (creating the device entry if new). Deduplicates; caps
// at DGM_MAX_GIDS; drops the add if the device table is full. Returns true iff
// the table changed.
bool dgm_tbl_add(DgmTable& t, uint64_t ieee, uint16_t gid);
// Remove gid from ieee's list; if that empties the device, drop its entry.
// Returns true iff the table changed.
bool dgm_tbl_remove(DgmTable& t, uint64_t ieee, uint16_t gid);
// Copy ieee's gids into out (at most max). Returns the number written.
uint8_t dgm_tbl_list(const DgmTable& t, uint64_t ieee, uint16_t* out, uint8_t max);
// Replace ieee's whole list (reconcile from a device readback). count 0 drops
// the device entry. Deduplicates + caps. Returns true iff the table changed.
bool dgm_tbl_set(DgmTable& t, uint64_t ieee, const uint16_t* gids, uint8_t count);
// Drop a device entirely (on device delete). Returns true iff present.
bool dgm_tbl_forget(DgmTable& t, uint64_t ieee);

// ── NVS-backed store ("zhac_gm" namespace) — thin load / op / save wrappers ──
void    dgm_store_init();
bool    dgm_add(uint64_t ieee, uint16_t gid);
bool    dgm_remove(uint64_t ieee, uint16_t gid);
uint8_t dgm_list(uint64_t ieee, uint16_t* out, uint8_t max);
bool    dgm_set(uint64_t ieee, const uint16_t* gids, uint8_t count);
bool    dgm_forget(uint64_t ieee);

// ── Global by-gid view (increment 3: the SPA "Groups" tab) ───────────────────
// Max distinct group ids the global view enumerates in one response.
static constexpr uint16_t DGM_MAX_GROUPS = 128;
// Serialize the whole table inverted by gid (ascending), members ieee-only:
//   {"groups":[{"gid":101,"members":[{"ieee":"0x..."},...]},...]}
// Pure (no NVS) so it is host-tested against a hand-built DgmTable.
size_t dgm_all_json(const DgmTable& t, char* buf, size_t cap);
// Load the mirror from NVS, then dgm_all_json. Runtime path for groups.all.
size_t dgm_all_json_live(char* buf, size_t cap);
