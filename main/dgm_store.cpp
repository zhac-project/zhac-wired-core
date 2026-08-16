// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PROVENANCE: copied verbatim from zhac-net-core/main/. Both apps need the same
// per-device group mirror and there is no shared component for it yet. Keep the
// two in sync, or promote this to zhac-components -- do NOT let them drift, the
// SPA talks the same protocol to both.
// dgm_store — per-device ZCL group-membership mirror. Pure table ops + a thin
// NVS-backed store (one "map" blob in the "zhac_gm" namespace).
#include "dgm_store.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include "json_buf.h"

// ── Pure table operations ────────────────────────────────────────────────────
static DgmEntry* dgm_find(DgmTable& t, uint64_t ieee) {
    for (uint8_t i = 0; i < t.n; i++) if (t.devs[i].ieee == ieee) return &t.devs[i];
    return nullptr;
}
static bool dgm_has(const DgmEntry& e, uint16_t gid) {
    for (uint8_t i = 0; i < e.count; i++) if (e.gids[i] == gid) return true;
    return false;
}

bool dgm_tbl_add(DgmTable& t, uint64_t ieee, uint16_t gid) {
    DgmEntry* e = dgm_find(t, ieee);
    if (e) {
        if (dgm_has(*e, gid)) return false;            // dedup
        if (e->count >= DGM_MAX_GIDS) return false;    // per-device cap
        e->gids[e->count++] = gid;
        return true;
    }
    if (t.n >= DGM_MAX_DEVS) return false;             // device table full
    DgmEntry& ne = t.devs[t.n];
    ne.ieee = ieee; ne.count = 0;
    ne.gids[ne.count++] = gid;
    t.n++;
    return true;
}

bool dgm_tbl_forget(DgmTable& t, uint64_t ieee) {
    for (uint8_t i = 0; i < t.n; i++) {
        if (t.devs[i].ieee == ieee) {
            t.devs[i] = t.devs[t.n - 1];   // swap-with-last
            t.n--;
            return true;
        }
    }
    return false;
}

bool dgm_tbl_remove(DgmTable& t, uint64_t ieee, uint16_t gid) {
    DgmEntry* e = dgm_find(t, ieee);
    if (!e) return false;                              // device not tracked
    for (uint8_t j = 0; j < e->count; j++) {
        if (e->gids[j] != gid) continue;
        for (uint8_t k = j; k + 1 < e->count; k++) e->gids[k] = e->gids[k + 1];
        e->count--;
        if (e->count == 0) dgm_tbl_forget(t, ieee);    // drop empty device entry
        return true;
    }
    return false;                                      // gid not present
}

uint8_t dgm_tbl_list(const DgmTable& t, uint64_t ieee, uint16_t* out, uint8_t max) {
    for (uint8_t i = 0; i < t.n; i++) {
        if (t.devs[i].ieee != ieee) continue;
        const DgmEntry& e = t.devs[i];
        uint8_t n = e.count < max ? e.count : max;
        for (uint8_t j = 0; j < n; j++) out[j] = e.gids[j];
        return n;
    }
    return 0;
}

bool dgm_tbl_set(DgmTable& t, uint64_t ieee, const uint16_t* gids, uint8_t count) {
    // Replace the device's list with the given gids (reconcile from a device
    // readback). Implemented as forget-then-add so dedup + cap are reused;
    // returns true if anything changed.
    bool changed = dgm_tbl_forget(t, ieee);
    for (uint8_t i = 0; i < count; i++) changed |= dgm_tbl_add(t, ieee, gids[i]);
    return changed;
}

// ── NVS-backed store ─────────────────────────────────────────────────────────
static constexpr const char* DGM_NS = "zhac_gm";
static SemaphoreHandle_t s_mtx = nullptr;
static inline void dgm_lock_init() { if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex(); }
namespace {
struct Guard { Guard() { dgm_lock_init(); if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
               ~Guard() { if (s_mtx) xSemaphoreGiveRecursive(s_mtx); } };
}

static void dgm_load(DgmTable& t) {
    memset(&t, 0, sizeof(t));
    nvs_handle_t h = 0;
    if (nvs_open(DGM_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(t);
    esp_err_t err = nvs_get_blob(h, "map", &t, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(t)) memset(&t, 0, sizeof(t));
    if (t.n > DGM_MAX_DEVS) t.n = DGM_MAX_DEVS;   // clamp a corrupt/stale blob
}
static bool dgm_save(const DgmTable& t) {
    nvs_handle_t h = 0;
    if (nvs_open(DGM_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "map", &t, sizeof(t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void dgm_store_init() { dgm_lock_init(); }

bool dgm_add(uint64_t ieee, uint16_t gid) {
    Guard g; DgmTable t; dgm_load(t); dgm_tbl_add(t, ieee, gid); return dgm_save(t);
}
bool dgm_remove(uint64_t ieee, uint16_t gid) {
    Guard g; DgmTable t; dgm_load(t); dgm_tbl_remove(t, ieee, gid); return dgm_save(t);
}
bool dgm_set(uint64_t ieee, const uint16_t* gids, uint8_t count) {
    Guard g; DgmTable t; dgm_load(t); dgm_tbl_set(t, ieee, gids, count); return dgm_save(t);
}
bool dgm_forget(uint64_t ieee) {
    Guard g; DgmTable t; dgm_load(t);
    bool had = dgm_tbl_forget(t, ieee);
    return had ? dgm_save(t) : true;
}
uint8_t dgm_list(uint64_t ieee, uint16_t* out, uint8_t max) {
    Guard g; DgmTable t; dgm_load(t); return dgm_tbl_list(t, ieee, out, max);
}

size_t dgm_all_json(const DgmTable& t, char* buf, size_t cap) {
    // 1. Collect distinct gids, insertion-sorted ascending, deduped, capped.
    uint16_t gids[DGM_MAX_GROUPS];
    uint16_t ng = 0;
    for (uint8_t d = 0; d < t.n; d++) {
        const DgmEntry& e = t.devs[d];
        for (uint8_t k = 0; k < e.count; k++) {
            uint16_t g = e.gids[k];
            uint16_t pos = 0;
            while (pos < ng && gids[pos] < g) pos++;
            if (pos < ng && gids[pos] == g) continue;          // dedup
            if (ng >= DGM_MAX_GROUPS) continue;                // cap (drop overflow)
            for (uint16_t m = ng; m > pos; m--) gids[m] = gids[m - 1];
            gids[pos] = g; ng++;
        }
    }
    // 2. Serialize by gid; for each, scan the table for member devices.
    JsonWriter w(buf, cap);
    w.raw("{\"groups\":[");
    for (uint16_t i = 0; i < ng; i++) {
        if (i) w.ch(',');
        w.fmt("{\"gid\":%u,\"members\":[", (unsigned)gids[i]);
        bool first = true;
        for (uint8_t d = 0; d < t.n; d++) {
            const DgmEntry& e = t.devs[d];
            bool has = false;
            for (uint8_t k = 0; k < e.count; k++) if (e.gids[k] == gids[i]) { has = true; break; }
            if (!has) continue;
            if (!first) w.ch(',');
            w.fmt("{\"ieee\":\"0x%016llx\"}", (unsigned long long)e.ieee);
            first = false;
        }
        w.raw("]}");
    }
    w.raw("]}");
    return w.finish();
}

size_t dgm_all_json_live(char* buf, size_t cap) {
    Guard g; DgmTable t; dgm_load(t);
    return dgm_all_json(t, buf, cap);
}
