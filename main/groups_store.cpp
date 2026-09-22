// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// groups_store.cpp — NVS persistence layer for Zigbee device groups
#include "groups_store.h"
#include "json_buf.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>

static constexpr const char* GRP_NVS_NS = "zhac_grp";

// ── Cross-task serialisation (FINDINGS §6, P4-T29) ─────────────────────────
// `group.list` is remote-allow-listed, so it runs on BOTH the single httpd
// task (local REST/WS) AND task_remote (cloud-invoked). Those two contexts can
// therefore touch this store concurrently: a httpd `group.create`/`update`/
// `delete` mutating the bitmap+blobs while task_remote walks the same NVS
// namespace for a `group.list`, and — worse — the non-atomic next_id()→save()
// in api_group_create lets two concurrent creators pick the SAME slot id and
// clobber each other. NVS handles are not a substitute for this: each op opens
// its own handle, so the bitmap read-modify-write in save()/delete() is not
// atomic against a concurrent reader/writer.
//
// Mirror the wifi-scan fix (P2-T16): one store-wide mutex held across each
// whole operation, so every bitmap+blob sequence is atomic across task
// contexts. RECURSIVE so a compound op (grp_create = next_id + save) can hold
// the lock across both sub-calls without the next_id→save TOCTOU that let two
// creators race onto the same slot. Lazily created (first call is in
// single-task boot context).
static SemaphoreHandle_t s_grp_mtx = nullptr;
static inline void grp_lock_init() {
    if (!s_grp_mtx) s_grp_mtx = xSemaphoreCreateRecursiveMutex();
}
namespace {
struct GrpLockGuard {
    GrpLockGuard()  { grp_lock_init(); if (s_grp_mtx) xSemaphoreTakeRecursive(s_grp_mtx, portMAX_DELAY); }
    ~GrpLockGuard() { if (s_grp_mtx) xSemaphoreGiveRecursive(s_grp_mtx); }
};
}  // namespace

// Public lock accessors so a caller that loads into a SHARED scratch buffer
// (api_group_list's file-static `all[]`, reachable from both httpd and
// task_remote) can hold the store lock across BOTH the grp_load_all and the
// serialise-from-the-buffer loop — otherwise a second group.list on the other
// task could refill the same static mid-serialisation (torn read). Recursive,
// so the grp_load_all inside the held region re-takes cheaply.
void grp_store_lock()   { grp_lock_init(); if (s_grp_mtx) xSemaphoreTakeRecursive(s_grp_mtx, portMAX_DELAY); }
void grp_store_unlock() { if (s_grp_mtx) xSemaphoreGiveRecursive(s_grp_mtx); }

static nvs_handle_t grp_open(nvs_open_mode_t mode) {
    nvs_handle_t h = 0;
    nvs_open(GRP_NVS_NS, mode, &h);
    return h;
}

static void grp_key(char* out, uint16_t id) { snprintf(out, 8, "g%04x", id); }

static uint32_t grp_get_bmp(nvs_handle_t h) {
    uint32_t bmp = 0;
    nvs_get_u32(h, "bmp", &bmp);
    return bmp;
}

// CODEX M-06: create the store mutex once during boot, before the httpd /
// remote server tasks start. The lazy grp_lock_init() below is retained as a
// backstop, but relying on it alone let two concurrent first-requests each
// see a null handle and create (then leak) a second mutex.
void grp_store_init() { grp_lock_init(); }

uint16_t grp_next_id() {
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READONLY);
    uint32_t bmp = h ? grp_get_bmp(h) : 0;
    if (h) nvs_close(h);
    for (uint8_t i = 0; i < GRP_MAX_GROUPS; i++) {
        if (!(bmp & (1u << i))) return (uint16_t)(i + 1);
    }
    return 0;  // all slots full
}

uint16_t grp_load_all(GrpRecord* out, uint16_t max) {
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READONLY);
    if (!h) return 0;
    uint32_t bmp = grp_get_bmp(h);
    uint16_t loaded = 0;
    for (uint8_t i = 0; i < GRP_MAX_GROUPS && loaded < max; i++) {
        if (!(bmp & (1u << i))) continue;
        uint16_t id = (uint16_t)(i + 1);
        char key[8]; grp_key(key, id);
        GrpRecord r{};
        size_t sz = sizeof(r);
        if (nvs_get_blob(h, key, &r, &sz) == ESP_OK && sz == sizeof(r) && r.id == id) {
            // CODEX M-06: validate the persisted invariant before use — a
            // corrupt/stale blob must not carry member_count > the array bound.
            if (r.member_count > GRP_MAX_MEMBERS) r.member_count = GRP_MAX_MEMBERS;
            out[loaded++] = r;
        }
    }
    nvs_close(h);
    return loaded;
}

bool grp_save(const GrpRecord& r) {
    if (r.id == 0 || r.id > GRP_MAX_GROUPS) return false;
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READWRITE);
    if (!h) return false;
    char key[8]; grp_key(key, r.id);
    esp_err_t err = nvs_set_blob(h, key, &r, sizeof(r));
    if (err == ESP_OK) {
        uint32_t bmp = grp_get_bmp(h);
        bmp |= (1u << (r.id - 1));
        nvs_set_u32(h, "bmp", bmp);
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool grp_create(GrpRecord& r) {
    // Atomic id-allocation + persist: hold the store lock across BOTH the
    // next_id() probe and the save() so two concurrent creators (httpd +
    // task_remote) cannot read the same free slot and clobber one another.
    // (Recursive mutex — the inner grp_next_id/grp_save re-take it cheaply.)
    GrpLockGuard guard;
    r.id = grp_next_id();
    if (r.id == 0) return false;   // table full
    return grp_save(r);
}

bool grp_delete(uint16_t id) {
    if (id == 0 || id > GRP_MAX_GROUPS) return false;
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READWRITE);
    if (!h) return false;
    char key[8]; grp_key(key, id);
    // CODEX M-06: propagate NVS failures instead of reporting only the final
    // commit — a failed blob erase or bitmap write otherwise leaves the bitmap
    // and blobs divergent while the call still returns success.
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   // already absent — delete is idempotent
    if (err == ESP_OK) {
        uint32_t bmp = grp_get_bmp(h);
        bmp &= ~(1u << (id - 1));
        err = nvs_set_u32(h, "bmp", bmp);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool grp_find(uint16_t id, GrpRecord& out) {
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READONLY);
    if (!h) return false;
    char key[8]; grp_key(key, id);
    size_t sz = sizeof(out);
    esp_err_t err = nvs_get_blob(h, key, &out, &sz);
    nvs_close(h);
    return err == ESP_OK && out.id == id;
}

size_t grp_to_json(const GrpRecord& r, char* buf, size_t cap) {
    // CODEX H-01: all appends go through JsonWriter, which keeps pos <= cap and
    // fails closed on overflow (returns 0) instead of walking off the buffer.
    JsonWriter w(buf, cap);
    w.fmt("{\"id\":%u,\"name\":\"", r.id);
    w.str(r.name, sizeof(r.name));   // bounded: a stale/corrupt blob's name may lack a NUL
    w.raw("\",\"members\":[");
    // Clamp defensively — a corrupt/stale NVS blob must not index past
    // members[GRP_MAX_MEMBERS].
    uint8_t mc = r.member_count > GRP_MAX_MEMBERS ? GRP_MAX_MEMBERS : r.member_count;
    for (uint8_t i = 0; i < mc; i++) {
        if (i) w.ch(',');
        w.fmt("{\"ieee\":\"0x%016llX\",\"ep\":%u}",
              (unsigned long long)r.members[i].ieee, r.members[i].ep);
    }
    w.raw("]}");
    size_t len = w.finish();
    // NUL-terminate the output. api_group_list accumulates the list with
    // JsonWriter::raw() and other consumers may use %s / strlen; without a
    // terminator (the writer's last op is a put(), which writes no NUL) they
    // over-read past the JSON into uninitialized memory and inject garbage into
    // the WS frame. finish() returns 0 on overflow → buf[0]='\0' is a valid
    // empty C-string. (Callers that copy by the returned length are unaffected.)
    if (len < cap) buf[len] = '\0';
    return len;
}
