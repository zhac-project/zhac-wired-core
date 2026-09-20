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
#include "auth.h"
#include "api_devices.h"
#include "device_options.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ArduinoJson.h"

#include "radio_state.h"
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
    // Copy the pool under its lock, then serialize and send from the copy:
    // a slow HTTP client must never hold up device reports and rules.
    zigbee_pool_lock();
    const uint16_t cnt = pool_count();
    ZapDevice* snap = cnt ? static_cast<ZapDevice*>(
        heap_caps_malloc(sizeof(ZapDevice) * cnt, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
    uint16_t n_snap = 0;
    if (snap) {
        ZapDevice* pool = pool_all();
        for (uint16_t i = 0; i < cnt; i++)
            if (!zap_dev_is_removed(&pool[i])) snap[n_snap++] = pool[i];
    }
    zigbee_pool_unlock();
    if (cnt && !snap) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < n_snap; i++) {
        const ZapDevice& d = snap[i];

        JsonDocument doc;
        char ieee_s[19];
        fmt_ieee(d.ieee_addr, ieee_s, sizeof(ieee_s));
        doc["ieee"]         = ieee_s;
        doc["nwk"]          = d.nwk_addr;
        doc["friendly"]     = d.friendly_name;
        // Friendly definition labels with raw fallback, matching the WS
        // device.list contract — the SPA's device table reads `vendor`, so a
        // row carrying only `manufacturer` renders that column as "—".
        char vendor_buf[32] = {};
        char model_buf[32]  = {};
        zhac_adapter_resolve_labels(d.model_id, d.manufacturer_name,
                                    vendor_buf, sizeof(vendor_buf),
                                    model_buf,  sizeof(model_buf));
        doc["model"]        = model_buf[0] ? (const char*)model_buf
                                           : (const char*)d.model_id;
        doc["vendor"]       = vendor_buf[0] ? (const char*)vendor_buf
                                            : (const char*)d.manufacturer_name;
        doc["manufacturer"] = d.manufacturer_name;
        doc["model_id"]     = d.model_id;
        doc["last_seen"]    = d.last_seen;
        // Parity with the WS device.list row, which already carried these.
        // A REST consumer had no way to see signal strength, battery or
        // endpoint count at all.
        doc["lqi"]          = d.link_quality;
        doc["battery"]      = d.battery_pct;
        doc["power_source"] = d.power_source;
        doc["ep_count"]     = d.endpoint_count;

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
            // Underscore-prefixed keys are shadow-internal bookkeeping
            // (`_last_seen`), not device state. The WS encoder has always
            // filtered them; this one did not, so REST consumers saw a phantom
            // "_last_seen" attribute alongside the real ones.
            if (sa[j].key[0] == '_') continue;
            switch (sa[j].val_type) {
                case VAL_INT:
                case VAL_BOOL: attrs[sa[j].key] = sa[j].int_val; break;
                case VAL_STR:  attrs[sa[j].key] = sa[j].str_val; break;
                // Decimals are stored x100 (zcl_attribute.h); the WS list divides,
                // this one did not, so REST showed 2150 for 21.5.
                case VAL_FLOAT: attrs[sa[j].key] = static_cast<float>(sa[j].int_val) / 100.0f; break;
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
    heap_caps_free(snap);
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
    // radio_permit_join, not zigbee_permit_join: it records the deadline the
    // WS `zigbee.permit_join.status` poll reads. Calling the backend directly
    // here would open the network without the UI ever learning it is open.
    bool ok = radio_permit_join((uint8_t)duration);
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
    const double value = doc["value"] | 0.0;   // integral or decimal; see send_number
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
    // Copy what the send needs and release the pool: the radio dispatch
    // blocks, and device reports must not wait on it (same as ws_bridge).
    const uint64_t ieee_cp = dev->ieee_addr;
    const uint16_t nwk_cp  = dev->nwk_addr;
    const uint8_t  ep      = dev->endpoints[0] ? dev->endpoints[0] : 1;
    char model_cp[64], manu_cp[64];
    snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
    snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
    zigbee_pool_unlock();
    bool ok = zhac_adapter_send_number(ieee_cp, model_cp, manu_cp, nwk_cp, ep, key, value);

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
    auth_register_uri(hd, &u);

    u.uri = "/api/permit_join"; u.method = HTTP_POST;
    u.handler = handle_permit_join;
    auth_register_uri(hd, &u);

    u.uri = "/api/device_state"; u.method = HTTP_POST;
    u.handler = handle_device_state;
    auth_register_uri(hd, &u);

    u.uri = "/api/device/reinterview"; u.method = HTTP_POST;
    u.handler = handle_device_reinterview;
    auth_register_uri(hd, &u);

    u.uri = "/api/device/configure"; u.method = HTTP_POST;
    u.handler = handle_device_configure;
    auth_register_uri(hd, &u);

    u.uri = "/api/device/options"; u.method = HTTP_POST;
    u.handler = handle_device_options;
    auth_register_uri(hd, &u);

    // Net-core URI aliases (the shared SPA targets these paths).
    u.uri = "/api/device/list";        u.method = HTTP_GET;  u.handler = handle_get_devices;    auth_register_uri(hd, &u);
    u.uri = "/api/device/attr/set";    u.method = HTTP_POST; u.handler = handle_device_state;   auth_register_uri(hd, &u);
    u.uri = "/api/device/options/set"; u.method = HTTP_POST; u.handler = handle_device_options; auth_register_uri(hd, &u);
    u.uri = "/api/zigbee/permit_join"; u.method = HTTP_POST; u.handler = handle_permit_join;    auth_register_uri(hd, &u);

    ESP_LOGI(TAG, "GET /api/devices, POST /api/permit_join, "
                  "POST /api/device_state, "
                  "POST /api/device/reinterview, "
                  "POST /api/device/configure registered");
    return true;
}
