// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// groups_store.cpp — NVS persistence layer for Zigbee device groups.
// Copied from zhac-net-core/main/groups_store.cpp (NVS-only, no P4).
// DIVERGENCE: grp_to_json advances `pos` by json_write_str()'s return so the
// group name is not clobbered — net-core's copy drops the name (its
// json_write_str return is ignored). That is a latent bug in net-core's
// groups_store.cpp:117 worth fixing there separately (sibling = read-only here).
#include "groups_store.h"
#include "nvs_flash.h"
#include <cstdio>
#include <cstring>

static constexpr const char* GRP_NVS_NS = "zhac_grp";

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

// Write a JSON-escaped string into buf[pos..cap), returning bytes written.
static size_t json_write_str(const char* s, char* buf, size_t pos, size_t cap) {
    size_t start = pos;
    for (const char* p = s; *p && pos < cap; p++) {
        unsigned char c = *p;
        if (c == '"')       { buf[pos++] = '\\'; buf[pos++] = '"';  }
        else if (c == '\\') { buf[pos++] = '\\'; buf[pos++] = '\\'; }
        else if (c == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n';  }
        else if (c == '\r') { buf[pos++] = '\\'; buf[pos++] = 'r';  }
        else if (c == '\t') { buf[pos++] = '\\'; buf[pos++] = 't';  }
        else if (c < 0x20)  {
            int n = snprintf(buf + pos, cap - pos, "\\u%04x", c);
            if (n < 0 || (size_t)n > cap - pos) break;
            pos += (size_t)n;
        }
        else { buf[pos++] = c; }
    }
    return pos - start;
}

uint16_t grp_next_id() {
    nvs_handle_t h = grp_open(NVS_READONLY);
    uint32_t bmp = h ? grp_get_bmp(h) : 0;
    if (h) nvs_close(h);
    for (uint8_t i = 0; i < GRP_MAX_GROUPS; i++) {
        if (!(bmp & (1u << i))) return (uint16_t)(i + 1);
    }
    return 0;  // all slots full
}

uint16_t grp_load_all(GrpRecord* out, uint16_t max) {
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
        if (nvs_get_blob(h, key, &r, &sz) == ESP_OK && r.id == id)
            out[loaded++] = r;
    }
    nvs_close(h);
    return loaded;
}

bool grp_save(const GrpRecord& r) {
    if (r.id == 0 || r.id > GRP_MAX_GROUPS) return false;
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

bool grp_delete(uint16_t id) {
    if (id == 0 || id > GRP_MAX_GROUPS) return false;
    nvs_handle_t h = grp_open(NVS_READWRITE);
    if (!h) return false;
    char key[8]; grp_key(key, id);
    nvs_erase_key(h, key);
    uint32_t bmp = grp_get_bmp(h);
    bmp &= ~(1u << (id - 1));
    nvs_set_u32(h, "bmp", bmp);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool grp_find(uint16_t id, GrpRecord& out) {
    nvs_handle_t h = grp_open(NVS_READONLY);
    if (!h) return false;
    char key[8]; grp_key(key, id);
    size_t sz = sizeof(out);
    esp_err_t err = nvs_get_blob(h, key, &out, &sz);
    nvs_close(h);
    return err == ESP_OK && out.id == id;
}

size_t grp_to_json(const GrpRecord& r, char* buf, size_t cap) {
    size_t pos = 0;
    pos += snprintf(buf + pos, cap - pos, "{\"id\":%u,\"name\":\"", r.id);
    pos += json_write_str(r.name, buf, pos, cap);   // fix: advance past name
    pos += snprintf(buf + pos, cap - pos, "\",\"members\":[");
    for (uint8_t i = 0; i < r.member_count && pos < cap; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, cap - pos, "{\"ieee\":\"0x%016llX\",\"ep\":%u}",
                        (unsigned long long)r.members[i].ieee, r.members[i].ep);
    }
    pos += snprintf(buf + pos, cap - pos, "]}");
    return pos < cap ? pos : 0;
}
