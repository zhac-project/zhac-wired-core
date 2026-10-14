// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// PROVENANCE: copied verbatim from zhac-net-core/main/. Both apps need the same
// per-device group mirror and there is no shared component for it yet. Keep the
// two in sync, or promote this to zhac-components -- do NOT let them drift, the
// SPA talks the same protocol to both.
//
// dgm_store — per-device, per-endpoint ZCL group-membership mirror. Pure table
// ops + a thin NVS-backed store (one "map2" blob in the "zhac_gm" namespace;
// the pre-endpoint "map" blob is migrated on first load).
#include "dgm_store.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include "json_buf.h"

// ── Pure table operations ────────────────────────────────────────────────────
static DgmEntry* dgm_find(DgmTable& t, uint64_t ieee, uint8_t ep) {
    for (uint8_t i = 0; i < t.n; i++)
        if (t.devs[i].ieee == ieee && t.devs[i].ep == ep) return &t.devs[i];
    return nullptr;
}
static bool dgm_has(const DgmEntry& e, uint16_t gid) {
    for (uint8_t i = 0; i < e.count; i++) if (e.gids[i] == gid) return true;
    return false;
}
static void dgm_drop(DgmTable& t, uint8_t i) {
    t.devs[i] = t.devs[t.n - 1];   // swap-with-last
    t.n--;
}

bool dgm_tbl_add(DgmTable& t, uint64_t ieee, uint8_t ep, uint16_t gid) {
    DgmEntry* e = dgm_find(t, ieee, ep);
    if (e) {
        if (dgm_has(*e, gid)) return false;            // dedup
        if (e->count >= DGM_MAX_GIDS) return false;    // per-endpoint cap
        e->gids[e->count++] = gid;
        return true;
    }
    if (t.n >= DGM_MAX_DEVS) return false;             // table full
    DgmEntry& ne = t.devs[t.n];
    ne.ieee = ieee; ne.ep = ep; ne.count = 0;
    ne.gids[ne.count++] = gid;
    t.n++;
    return true;
}

bool dgm_tbl_forget(DgmTable& t, uint64_t ieee) {
    bool had = false;
    for (uint8_t i = 0; i < t.n;) {
        if (t.devs[i].ieee == ieee) { dgm_drop(t, i); had = true; }   // re-check the swapped-in entry
        else i++;
    }
    return had;
}

bool dgm_tbl_remove(DgmTable& t, uint64_t ieee, uint8_t ep, uint16_t gid) {
    for (uint8_t i = 0; i < t.n; i++) {
        DgmEntry& e = t.devs[i];
        if (e.ieee != ieee || e.ep != ep) continue;
        for (uint8_t j = 0; j < e.count; j++) {
            if (e.gids[j] != gid) continue;
            for (uint8_t k = j; k + 1 < e.count; k++) e.gids[k] = e.gids[k + 1];
            e.count--;
            if (e.count == 0) dgm_drop(t, i);           // drop an empty entry
            return true;
        }
        return false;                                  // gid not present
    }
    return false;                                      // not tracked
}

uint8_t dgm_tbl_list(const DgmTable& t, uint64_t ieee, uint16_t* out, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < t.n; i++) {
        const DgmEntry& e = t.devs[i];
        if (e.ieee != ieee) continue;
        for (uint8_t j = 0; j < e.count && n < max; j++) {
            bool seen = false;                         // a group on two endpoints, once
            for (uint8_t k = 0; k < n; k++) if (out[k] == e.gids[j]) { seen = true; break; }
            if (!seen) out[n++] = e.gids[j];
        }
    }
    return n;
}

bool dgm_tbl_set(DgmTable& t, uint64_t ieee, uint8_t ep, const uint16_t* gids, uint8_t count) {
    // Replace this endpoint's list (reconcile from a device readback). Remove
    // then add, so dedup + cap are reused; returns true if anything changed.
    bool changed = false;
    for (uint8_t i = 0; i < t.n; i++)
        if (t.devs[i].ieee == ieee && t.devs[i].ep == ep) { dgm_drop(t, i); changed = true; break; }
    for (uint8_t i = 0; i < count; i++) changed |= dgm_tbl_add(t, ieee, ep, gids[i]);
    return changed;
}

void dgm_tbl_from_v1(const DgmTableV1& old, DgmTable& out) {
    std::memset(&out, 0, sizeof(out));
    const uint8_t n = old.n > DGM_MAX_DEVS ? DGM_MAX_DEVS : old.n;   // clamp a corrupt blob
    for (uint8_t i = 0; i < n; i++) {
        const DgmEntryV1& o = old.devs[i];
        const uint8_t c = o.count > DGM_MAX_GIDS ? DGM_MAX_GIDS : o.count;
        for (uint8_t j = 0; j < c; j++) dgm_tbl_add(out, o.ieee, 1, o.gids[j]);
    }
}

// ── NVS-backed store ─────────────────────────────────────────────────────────
static constexpr const char* DGM_NS = "zhac_gm";
static SemaphoreHandle_t s_mtx = nullptr;
static inline void dgm_lock_init() { if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex(); }
namespace {
struct Guard { Guard() { dgm_lock_init(); if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
               ~Guard() { if (s_mtx) xSemaphoreGiveRecursive(s_mtx); } };
}

static bool dgm_save(const DgmTable& t) {
    nvs_handle_t h = 0;
    if (nvs_open(DGM_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "map2", &t, sizeof(t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// The pre-endpoint "map" blob, once: its entries move to endpoint 1 and the
// old key goes only after the new one is saved.
static bool dgm_load_v1(nvs_handle_t h, DgmTable& t) {
    DgmTableV1* old = new (std::nothrow) DgmTableV1{};   // ~3 KB: not on the caller's stack
    if (!old) return false;
    size_t sz = sizeof(*old);
    const bool ok = nvs_get_blob(h, "map", old, &sz) == ESP_OK && sz == sizeof(*old);
    if (ok) dgm_tbl_from_v1(*old, t);
    delete old;
    return ok;
}

static void dgm_load(DgmTable& t) {
    memset(&t, 0, sizeof(t));
    nvs_handle_t h = 0;
    if (nvs_open(DGM_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(t);
    esp_err_t err = nvs_get_blob(h, "map2", &t, &sz);
    bool migrated = false;
    if (err != ESP_OK || sz != sizeof(t)) {
        memset(&t, 0, sizeof(t));
        migrated = dgm_load_v1(h, t);
    }
    nvs_close(h);
    if (t.n > DGM_MAX_DEVS) t.n = DGM_MAX_DEVS;   // clamp a corrupt/stale blob
    if (migrated && dgm_save(t) && nvs_open(DGM_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "map");
        nvs_commit(h);
        nvs_close(h);
    }
}

void dgm_store_init() { dgm_lock_init(); }

// Load, run f on the table, under the lock. The table is ~3 KB and these run
// on HTTP / WebSocket tasks, so it lives on the heap.
template <class F>
static auto with_table(F f) -> decltype(f(std::declval<DgmTable&>())) {
    Guard g;
    std::unique_ptr<DgmTable> t(new (std::nothrow) DgmTable{});
    if (!t) return {};
    dgm_load(*t);
    return f(*t);
}

bool dgm_add(uint64_t ieee, uint8_t ep, uint16_t gid) {
    return with_table([&](DgmTable& t) { dgm_tbl_add(t, ieee, ep, gid); return dgm_save(t); });
}
bool dgm_remove(uint64_t ieee, uint8_t ep, uint16_t gid) {
    return with_table([&](DgmTable& t) { dgm_tbl_remove(t, ieee, ep, gid); return dgm_save(t); });
}
bool dgm_set(uint64_t ieee, uint8_t ep, const uint16_t* gids, uint8_t count) {
    return with_table([&](DgmTable& t) { dgm_tbl_set(t, ieee, ep, gids, count); return dgm_save(t); });
}
bool dgm_forget(uint64_t ieee) {
    return with_table([&](DgmTable& t) { return dgm_tbl_forget(t, ieee) ? dgm_save(t) : true; });
}
uint8_t dgm_list(uint64_t ieee, uint16_t* out, uint8_t max) {
    return with_table([&](DgmTable& t) { return dgm_tbl_list(t, ieee, out, max); });
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
    // 2. Serialize by gid; for each, scan the table for member device endpoints.
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
            w.fmt("{\"ieee\":\"0x%016llx\",\"ep\":%u}", (unsigned long long)e.ieee, (unsigned)e.ep);
            first = false;
        }
        w.raw("]}");
    }
    w.raw("]}");
    return w.finish();
}

size_t dgm_all_json_live(char* buf, size_t cap) {
    return with_table([&](DgmTable& t) { return dgm_all_json(t, buf, cap); });
}
