// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// api_devices.cpp — device REST surface (read + admin).
//
//   GET  /api/devices                    — list pool + shadow attrs
//   POST /api/permit_join                — open network ({"duration": N})
//   POST /api/device_state               — set attr ({"ieee":"0x..","key":"state","value":1})
//
// All direct in-process calls. No HAP, no roundtrip. JSON I/O via
// ArduinoJson against bounded buffers.
#include "api_devices.h"
#include "device_options.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ArduinoJson.h"

#include "zigbee_mgr.h"
#include "zigbee_pool.h"
#include "device_shadow.h"
#include "zhc_adapter.h"
#include "zap_common.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

static const char* TAG = "api_devices";

// Helper — parse "0x001234567890ABCD" → uint64_t. Returns 0 on bad input.
static uint64_t parse_ieee(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint64_t)strtoull(s, nullptr, 16);
}

// Helper — format IEEE as "0xHHHHHHHHHHHHHHHH".
static void fmt_ieee(uint64_t ieee, char* out, size_t cap) {
    snprintf(out, cap, "0x%016" PRIX64, ieee);
}

// ── GET /api/devices ────────────────────────────────────────────────────
//
// Streams the device list directly to the response — the JSON can grow
// past 4 KB once attrs are emitted, so we don't buffer the entire doc.
// One device per chunk keeps peak heap small.
static esp_err_t handle_get_devices(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");

    zigbee_pool_lock();
    ZapDevice* pool = pool_all();
    uint16_t cnt = pool_count();

    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < cnt; i++) {
        const ZapDevice& d = pool[i];
        if (zap_dev_is_removed(&d)) continue;

        JsonDocument doc;
        char ieee_s[19];
        fmt_ieee(d.ieee_addr, ieee_s, sizeof(ieee_s));
        doc["ieee"]         = ieee_s;
        doc["nwk"]          = d.nwk_addr;
        doc["friendly"]     = d.friendly_name;
        doc["model"]        = d.model_id;
        doc["manufacturer"] = d.manufacturer_name;
        doc["last_seen"]    = d.last_seen;

        // Endpoints array
        JsonArray eps = doc["endpoints"].to<JsonArray>();
        for (uint8_t e = 0; e < d.endpoint_count && e < 8; e++) {
            eps.add(d.endpoints[e]);
        }

        // Shadow attrs — at most 32 per device (matches Lua's view).
        ShadowAttr sa[32];
        uint8_t n = device_shadow_get_attrs(d.ieee_addr, sa, 32);
        JsonObject attrs = doc["attrs"].to<JsonObject>();
        for (uint8_t j = 0; j < n; j++) {
            switch (sa[j].val_type) {
                case VAL_INT:
                case VAL_BOOL: attrs[sa[j].key] = sa[j].int_val; break;
                case VAL_STR:  attrs[sa[j].key] = sa[j].str_val; break;
                default: break;
            }
        }

        char chunk[1024];
        size_t len = serializeJson(doc, chunk, sizeof(chunk));
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        httpd_resp_send_chunk(req, chunk, len);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, nullptr);  // terminate chunked response
    zigbee_pool_unlock();
    return ESP_OK;
}

// ── POST /api/device/reinterview ────────────────────────────────────────
//
// Body: {"ieee":"0x..."}. Re-runs the full ZNP interview (Active EP,
// Simple Desc, Basic-cluster reads); when interview completes the
// zigbee_configure_queue picks up the device and fires
// `zhac_adapter_configure` which walks the def's bindings + reports
// + config_steps (read-on-join etc.).
//
// Use for: paired devices that joined before the def gained a
// configure pipeline (e.g. ZG-204Z paired before sensitivity /
// keep_time landed) — they won't auto-refire configure on flash
// since they never re-join. The SPA "Reinterview" button hits this.
static esp_err_t handle_device_reinterview(httpd_req_t* req) {
    char buf[64];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char* ieee_s = doc["ieee"] | (const char*)nullptr;
    if (!ieee_s || ieee_s[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_FAIL;
    }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ieee");
        return ESP_FAIL;
    }
    const bool ok = zigbee_interview_trigger(ieee);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        ESP_LOGI(TAG, "interview retriggered for 0x%016" PRIX64, ieee);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"unknown or sleepy\"}");
}

// ── POST /api/device/configure ──────────────────────────────────────────
//
// Body: {"ieee":"0x..."}. Re-runs ONLY the configure pipeline (bindings +
// reports + config_steps) without redoing the full interview. Faster than
// reinterview when the def has just gained new config_steps and the
// device is awake. The interview-derived (model_id, manufacturer_name)
// must already be cached from the original join.
static esp_err_t handle_device_configure(httpd_req_t* req) {
    char buf[64];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char* ieee_s = doc["ieee"] | (const char*)nullptr;
    if (!ieee_s) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_FAIL;
    }
    uint64_t ieee = parse_ieee(ieee_s);

    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev || dev->model_id[0] == '\0') {
        zigbee_pool_unlock();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                             "device unknown or never interviewed");
        return ESP_FAIL;
    }
    // Snapshot — releasing the lock before the call since
    // zhac_adapter_configure walks the def and issues radio frames
    // (which may block on AF_DATA_CONFIRM up to a few seconds).
    const uint64_t  ieee_cp = dev->ieee_addr;
    const uint16_t  nwk_cp  = dev->nwk_addr;
    // Buffers sized to exceed ZapDevice::{model_id, manufacturer_name}
    // (~34 B each) — anything smaller trips -Werror=format-truncation.
    char model_cp[64];
    char manu_cp[64];
    snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
    snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
    zigbee_pool_unlock();

    const bool ok = zhac_adapter_configure(ieee_cp, nwk_cp,
                                            model_cp, manu_cp);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        ESP_LOGI(TAG, "configure re-fired for 0x%016" PRIX64, ieee_cp);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req,
        "{\"ok\":false,\"err\":\"no def or transport down\"}");
}

// ── POST /api/permit_join ───────────────────────────────────────────────
//
// Body: {"duration": 60}. Range checked against Zigbee spec (0–254).
static esp_err_t handle_permit_join(httpd_req_t* req) {
    char buf[64];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    int duration = doc["duration"] | -1;
    if (duration < 0 || duration > 254) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "duration 0-254");
        return ESP_FAIL;
    }
    bool ok = zigbee_permit_join((uint8_t)duration);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        char r[48];
        int rl = snprintf(r, sizeof(r),
                          "{\"ok\":true,\"duration\":%d}", duration);
        return httpd_resp_send(req, r, rl);
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "permit_join failed");
    return ESP_FAIL;
}

// ── POST /api/device_state ──────────────────────────────────────────────
//
// Body: {"ieee":"0x...","key":"state","value":1}. Sets a single
// numeric/bool attribute on a device via zhc_adapter. Use
// {"value":0} or {"value":1} for state/on_off; numeric keys (brightness,
// color_temp, …) accept their full range.
static esp_err_t handle_device_state(httpd_req_t* req) {
    char buf[256];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char* ieee_s = doc["ieee"]  | (const char*)nullptr;
    const char* key    = doc["key"]   | (const char*)nullptr;
    uint64_t    value  = doc["value"] | (uint64_t)0;
    if (!ieee_s || !key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee or key");
        return ESP_FAIL;
    }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ieee");
        return ESP_FAIL;
    }

    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) {
        zigbee_pool_unlock();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not found");
        return ESP_FAIL;
    }
    uint8_t ep = dev->endpoints[0] ? dev->endpoints[0] : 1;
    bool ok = zhac_adapter_send_uint(dev->ieee_addr,
                                      dev->model_id,
                                      dev->manufacturer_name,
                                      dev->nwk_addr, ep, key, value);
    zigbee_pool_unlock();

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "no zhc converter");
    return ESP_FAIL;
}

// ── POST /api/device/options ─────────────────────────────────────────────
//
// Body: {"ieee":"0x..","occupancy_timeout"?,"debounce_ms"?,
//        "flood_protection"?,"throttle_ms"?}. Per-device runtime options;
// applied to device_shadow directly (single chip — no DEVICE_OPTIONS_SET HAP
// hop) and persisted to NVS so they survive reboot.
static esp_err_t handle_device_options(httpd_req_t* req) {
    char buf[256];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    const char* ieee_s = doc["ieee"] | (const char*)nullptr;
    if (!ieee_s) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_FAIL;
    }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ieee");
        return ESP_FAIL;
    }

    bool ok = device_options_set(ieee, buf, (size_t)n);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "options apply failed");
    return ESP_FAIL;
}

bool api_devices_register(httpd_handle_t hd) {
    if (!hd) return false;
    httpd_uri_t u{};

    u.uri = "/api/devices"; u.method = HTTP_GET;
    u.handler = handle_get_devices;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/permit_join"; u.method = HTTP_POST;
    u.handler = handle_permit_join;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/device_state"; u.method = HTTP_POST;
    u.handler = handle_device_state;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/device/reinterview"; u.method = HTTP_POST;
    u.handler = handle_device_reinterview;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/device/configure"; u.method = HTTP_POST;
    u.handler = handle_device_configure;
    httpd_register_uri_handler(hd, &u);

    u.uri = "/api/device/options"; u.method = HTTP_POST;
    u.handler = handle_device_options;
    httpd_register_uri_handler(hd, &u);

    // Net-core URI aliases (the shared SPA targets these paths).
    u.uri = "/api/device/list";        u.method = HTTP_GET;  u.handler = handle_get_devices;    httpd_register_uri_handler(hd, &u);
    u.uri = "/api/device/attr/set";    u.method = HTTP_POST; u.handler = handle_device_state;   httpd_register_uri_handler(hd, &u);
    u.uri = "/api/device/options/set"; u.method = HTTP_POST; u.handler = handle_device_options; httpd_register_uri_handler(hd, &u);
    u.uri = "/api/zigbee/permit_join"; u.method = HTTP_POST; u.handler = handle_permit_join;    httpd_register_uri_handler(hd, &u);

    ESP_LOGI(TAG, "GET /api/devices, POST /api/permit_join, "
                  "POST /api/device_state, "
                  "POST /api/device/reinterview, "
                  "POST /api/device/configure registered");
    return true;
}
