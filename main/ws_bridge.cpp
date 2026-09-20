// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ws_bridge.cpp — WebSocket inbound dispatcher + outbound event push.
//
// Inbound protocol (SPA → firmware):
//   {"id":<seq>, "cmd":"<verb>", "args":{...}}
// Reply (firmware → originating SPA fd):
//   {"id":<seq>, "ok":<bool>, ...result fields...}
//
// Push protocol (firmware → all SPA clients on event_bus):
//   {"type":"zcl_attr",   ieee, ep, cluster, attr_id, key, value}
//   {"type":"device_join", ieee, friendly}
//   {"type":"device_leave", ieee}
//
// Phase 6 minimum verbs: `ping`, `status`. Extend by adding cases in
// dispatch() — the same shape is used by net-core's full WS bridge,
// so SPA code that targets net-core sees identical wire formats.

#include "ws_bridge.h"
#include "ws_server.h"
#include "sys_state.h"
#include "event_bus.h"
#include "esp_log.h"
#include "device_cmd.h"
#include "device_cmd_json.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "zigbee_mgr.h"
#include "device_backend.h"
#include "zap_clock.h"
#include "esp_zigbee_backend.h"   // esp_zigbee_backend_wall_clock_s
#include "sys_diag.h"
#include "dgm_store.h"
#include "zigbee_pool.h"
#include "zhc_adapter.h"
#include "zap_common.h"
#include "zap_store.h"
#include "device_shadow.h"
#include "mqtt_gw.h"
#include "ArduinoJson.h"
#include "sdkconfig.h"
#include "device_options.h"
#include "api_system.h"
#include "api_groups.h"
#include "api_status.h"
#include "auth.h"
#include "ota_update.h"
#include "eth.h"
#include "net_discovery.h"
#include "radio_state.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "log_ring.h"
#include "api_remote.h"
#include "remote_client.h"
#include "simple_rules.h"
#include "rule_store.h"
#include "zap_common.h"
#include "lua_engine.h"
#include "lua_engine_scripts.h"
#include "nvs.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <sys/time.h>
#include <cinttypes>

static const char* TAG = "ws_bridge";

// ── Inbound dispatch ────────────────────────────────────────────────────
static void send_err(int fd, uint32_t id, const char* err) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%" PRIu32 ",\"ok\":false,\"err\":\"%s\"}",
                     id, err);
    ws_server_reply(fd, buf, n);
}

// Forward declarations — defined lower in the file, but used by the
// settings / group command handlers placed above them.
static void     reply_ok_or_err(int fd, uint32_t id, bool ok, const char* err);
static uint64_t parse_ieee(const char* s);

// time.set {epoch}: the browser's clock for a hub that has none -- no RTC, and
// SNTP needs the internet. It fills an unset clock only (zap_clock.h), so a
// browser can start the schedules of an offline hub but never moves a clock
// SNTP has set. Reply data {"set":false} means the clock was already set.
static void cmd_time_set(int fd, uint32_t id, JsonDocument& doc) {
    const int64_t epoch = doc["args"]["epoch"] | static_cast<int64_t>(0);
    if (!zap_clock_epoch_ok(epoch)) { send_err(fd, id, "bad epoch"); return; }
    bool set = false;
    if (!zap_clock_is_set(time(nullptr))) {
        const timeval tv{static_cast<time_t>(epoch), 0};
        set = settimeofday(&tv, nullptr) == 0;
        if (set) ESP_LOGI(TAG, "clock set from the web UI");
    }
    char buf[64];
    const int n = snprintf(buf, sizeof(buf), "{\"id\":%" PRIu32 ",\"ok\":true,\"data\":{\"set\":%s}}",
                           id, set ? "true" : "false");
    ws_server_reply(fd, buf, n);
}

static void cmd_ping(int fd, uint32_t id) {
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%" PRIu32 ",\"ok\":true,\"pong\":true,"
                     "\"uptime_s\":%" PRIu32 "}",
                     id, (uint32_t)(esp_timer_get_time() / 1000000));
    ws_server_reply(fd, buf, n);
}

static void cmd_status(int fd, uint32_t id) {
    JsonDocument doc;
    doc["id"] = id;
    doc["ok"] = true;
    JsonObject d = doc["data"].to<JsonObject>();
    // Same builder as REST /api/status (full sys_diag set + network, MQTT,
    // auth and `sku`). This used to emit the diagnostics only, so the
    // Settings page showed every toggle off and the MQTT fields blank.
    api_status_fill(d);
    // Same size as the REST reply (api_status.cpp). serializeJson truncates
    // rather than failing, and a truncated reply is invalid JSON that the
    // page silently drops -- so refuse loudly instead.
    constexpr size_t CAP = 2560;
    char* buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { send_err(fd, id, "oom"); return; }
    size_t n = serializeJson(doc, buf, CAP);
    if (n == 0 || n >= CAP) {
        ESP_LOGE(TAG, "status.get overflow: %u B needed", (unsigned)measureJson(doc));
        heap_caps_free(buf);
        send_err(fd, id, "status too large");
        return;
    }
    ws_server_reply(fd, buf, n);
    heap_caps_free(buf);
}

// ── settings.set / token.rotate / diagnostics.unhandled ───────────────
// Thin WS wrappers over the shared api_system logic (same as the REST path).
static void cmd_settings_set(int fd, uint32_t id, JsonDocument& doc) {
    char args[512];
    size_t n = serializeJson(doc["args"], args, sizeof(args));
    bool ok = (n > 0 && n < sizeof(args)) && system_apply_settings(args, n);
    reply_ok_or_err(fd, id, ok, "bad settings");
}

static void cmd_token_rotate(int fd, uint32_t id) {
    char tok[33];
    if (!system_rotate_token(tok, sizeof(tok))) { send_err(fd, id, "nvs"); return; }
    JsonDocument d;
    d["id"] = id; d["ok"] = true; d["data"]["token"] = tok;
    char buf[96];
    size_t n = serializeJson(d, buf, sizeof(buf));
    ws_server_reply(fd, buf, n);
}

static void cmd_diagnostics_unhandled(int fd, uint32_t id) {
    char tmp[1408];
    size_t tn = system_diagnostics_json(tmp, sizeof(tmp));
    JsonDocument data;
    if (tn == 0 || deserializeJson(data, tmp, tn)) { data.clear(); data["entries"].to<JsonArray>(); }
    JsonDocument env; env["id"] = id; env["ok"] = true; env["data"] = data;
    char buf[1536];
    size_t n = serializeJson(env, buf, sizeof(buf));
    ws_server_reply(fd, buf, n);
}

// ── group.* ───────────────────────────────────────────────────────────
// Payloads nest under "group"/"groups" so they don't collide with the
// envelope's own "id"/"ok" keys (group records carry their own "id").
static void cmd_group_list(int fd, uint32_t id) {
    constexpr size_t CAP = 4096;            // PSRAM heap — keep off internal BSS
    char* tmp = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    char* buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!tmp || !buf) { heap_caps_free(tmp); heap_caps_free(buf); send_err(fd, id, "oom"); return; }
    size_t tn = group_list_json(tmp, CAP);
    JsonDocument inner; deserializeJson(inner, tmp, tn);
    JsonDocument env; env["id"] = id; env["ok"] = true; env["data"] = inner;
    size_t n = serializeJson(env, buf, CAP);
    ws_server_reply(fd, buf, n);
    heap_caps_free(tmp); heap_caps_free(buf);
}

static void cmd_group_create(int fd, uint32_t id, JsonDocument& doc) {
    char args[1024]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    char rsp[512]; size_t rn = 0;
    if (!group_create(args, an, rsp, sizeof(rsp), &rn)) { send_err(fd, id, "create failed"); return; }
    JsonDocument inner; deserializeJson(inner, rsp, rn);
    JsonDocument env; env["id"] = id; env["ok"] = true; env["data"] = inner;
    char buf[640]; size_t n = serializeJson(env, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
    ws_push("group.added", inner);
}
static void cmd_group_get(int fd, uint32_t id, JsonDocument& doc) {
    char args[256]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    char rsp[512]; size_t rn = 0;
    if (!group_get(args, an, rsp, sizeof(rsp), &rn)) { send_err(fd, id, "no such group"); return; }
    JsonDocument inner; deserializeJson(inner, rsp, rn);
    JsonDocument env; env["id"] = id; env["ok"] = true; env["data"] = inner;
    char buf[640]; size_t n = serializeJson(env, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
}
static void cmd_group_update(int fd, uint32_t id, JsonDocument& doc) {
    char args[1024]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    char rsp[512]; size_t rn = 0;
    if (!group_update(args, an, rsp, sizeof(rsp), &rn)) { send_err(fd, id, "no such group"); return; }
    JsonDocument inner; deserializeJson(inner, rsp, rn);
    JsonDocument env; env["id"] = id; env["ok"] = true; env["data"] = inner;
    char buf[640]; size_t n = serializeJson(env, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
    ws_push("group.updated", inner);
}
static void cmd_group_delete(int fd, uint32_t id, JsonDocument& doc) {
    char args[256]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    bool ok = group_delete_req(args, an);
    reply_ok_or_err(fd, id, ok, "delete failed");
    if (ok) { JsonDocument p; p["id"] = doc["args"]["id"] | (int)0; ws_push("group.deleted", p); }
}
static void cmd_group_cmd(int fd, uint32_t id, JsonDocument& doc) {
    char args[512]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    char rsp[128]; size_t rn = group_cmd(args, an, rsp, sizeof(rsp));
    JsonDocument inner; deserializeJson(inner, rsp, rn);
    JsonDocument env; env["id"] = id; env["ok"] = inner["ok"];
    env["sent"] = inner["sent"]; env["failed"] = inner["failed"];
    char buf[192]; size_t n = serializeJson(env, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
}

// ── net.* (and the wifi.* compatibility aliases) ──────────────────────
// This build is Ethernet-only. `net.status` is the wired-native command;
// `wifi.status` and `wifi.scan` stay registered and answer honestly so the
// shared www-spa keeps working unmodified, and the two write commands are
// refused rather than silently ignored. See api_net.cpp for the REST twins
// of exactly these payloads -- keep the two in step.
static void cmd_net_status(int fd, uint32_t id) {
    NetStatus st{}; eth_get_status(&st);
    JsonDocument d; d["id"] = id; d["ok"] = true;
    JsonObject da = d["data"].to<JsonObject>();
    da["transport"]  = "ethernet";
    da["link_up"]    = st.link_up;
    da["has_ip"]     = st.has_ip;
    da["ip"]         = st.ip;
    da["netmask"]    = st.netmask;
    da["gateway"]    = st.gw;
    da["mac"]        = st.mac;
    da["speed_mbps"] = st.speed_mbps;
    da["duplex"]     = st.duplex_full ? "full" : "half";
    da["hostname"]   = net_discovery_hostname();
    char buf[448]; size_t n = serializeJson(d, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
}
static void cmd_wifi_status(int fd, uint32_t id) {
    NetStatus st{}; eth_get_status(&st);
    JsonDocument d; d["id"] = id; d["ok"] = true;
    JsonObject da = d["data"].to<JsonObject>();
    da["mode"] = "eth";
    da["ssid"] = "";
    da["ip"] = st.ip; da["rssi"] = 0;
    da["sta_configured"] = st.link_up; da["sta_connected"] = st.has_ip;
    da["sta_ssid"] = ""; da["ap_ssid"] = "";
    char buf[384]; size_t n = serializeJson(d, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
}
static void cmd_wifi_scan(int fd, uint32_t id) {
    // Always empty rather than an error: a SPA that scans on page load should
    // render "no networks", not an error toast, on a box with no radio.
    JsonDocument d; d["id"] = id; d["ok"] = true;
    d["data"]["networks"].to<JsonArray>();
    char buf[128]; size_t n = serializeJson(d, buf, sizeof(buf)); ws_server_reply(fd, buf, n);
}
static void cmd_wifi_unsupported(int fd, uint32_t id) {
    send_err(fd, id, "wifi unsupported: zhac-wired-core is Ethernet-only");
}

// ── logs.get ──────────────────────────────────────────────────────────
// Envelope built by splicing log_ring_to_json's {"logs":[...]} after the
// {id,ok} prefix — avoids a multi-KB JsonDocument on the internal heap.
static void cmd_logs_get(int fd, uint32_t id) {
    constexpr size_t CAP = 32768;
    char* tmp = (char*)heap_caps_malloc(CAP,      MALLOC_CAP_SPIRAM);
    char* buf = (char*)heap_caps_malloc(CAP + 64, MALLOC_CAP_SPIRAM);
    if (!tmp || !buf) { heap_caps_free(tmp); heap_caps_free(buf); send_err(fd, id, "oom"); return; }
    size_t tn = log_ring_to_json(tmp, CAP);          // {"logs":[...]}
    int n;
    if (tn >= 2)
        n = snprintf(buf, CAP + 64, "{\"id\":%u,\"ok\":true,\"data\":%.*s}",
                     (unsigned)id, (int)tn, tmp);
    else
        n = snprintf(buf, CAP + 64, "{\"id\":%u,\"ok\":true,\"data\":{\"logs\":[]}}", (unsigned)id);
    ws_server_reply(fd, buf, (size_t)n);
    heap_caps_free(tmp); heap_caps_free(buf);
}

// Parse "0x001234567890ABCD" or bare hex → uint64_t. 0 on bad input.
static uint64_t parse_ieee(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint64_t)strtoull(s, nullptr, 16);
}

// Send a `{id,ok,...}` reply with a single boolean status; on
// failure, attach an `err` string for the SPA toast.
static void reply_ok_or_err(int fd, uint32_t id, bool ok, const char* err) {
    char buf[128];
    int n = ok
        ? snprintf(buf, sizeof(buf),
                   "{\"id\":%" PRIu32 ",\"ok\":true}", id)
        : snprintf(buf, sizeof(buf),
                   "{\"id\":%" PRIu32 ",\"ok\":false,\"err\":\"%s\"}",
                   id, err ? err : "failed");
    ws_server_reply(fd, buf, n);
}

// ── device.reinterview ────────────────────────────────────────────────
//
// SPA action: re-run the full ZNP interview (Active EP / Simple Desc /
// Basic-cluster reads). On completion `zigbee_configure_queue` picks up
// the device and fires `zhac_adapter_configure` which walks the def's
// bindings + reports + config_steps. Use when the device was paired
// before a definition gained new wiring (e.g. ZG-204Z's read-on-join
// for sensitivity / keep_time).
static void cmd_device_reinterview(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    if (!ieee_s || !ieee_s[0]) { send_err(fd, id, "missing ieee"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }
    const bool ok = zigbee_interview_trigger(ieee);
    reply_ok_or_err(fd, id, ok, "unknown or sleepy");
}

// ── device.configure ──────────────────────────────────────────────────
//
// Skips the interview and re-runs ONLY the configure pipeline (bindings
// + reports + config_steps). Faster than reinterview when the device's
// (model_id, manufacturer_name) are already cached. Useful right after
// a firmware update that added new `config_steps` for an existing
// paired device.
//
// Releases the pool lock before invoking `zhac_adapter_configure` —
// the call issues radio frames that can block on AF_DATA_CONFIRM for
// ~2.5 s; holding the pool mutex through that would stall every other
// device dispatching attrs simultaneously.
static void cmd_device_configure(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    if (!ieee_s) { send_err(fd, id, "missing ieee"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }

    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev || dev->model_id[0] == '\0') {
        zigbee_pool_unlock();
        send_err(fd, id, "device unknown or never interviewed");
        return;
    }
    const uint64_t ieee_cp = dev->ieee_addr;
    const uint16_t nwk_cp  = dev->nwk_addr;
    char model_cp[64];
    char manu_cp[64];
    snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
    snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
    zigbee_pool_unlock();

    const bool ok = zhac_adapter_configure(ieee_cp, nwk_cp, model_cp, manu_cp);

    // Record the outcome. Without this a successful manual retry leaves the
    // row still showing FAILED, so the operator has no way to tell whether
    // the button did anything.
    ZapDevice cur{};
    if (zigbee_pool_snapshot(ieee_cp, &cur)) {
        cur.configure_state = static_cast<uint8_t>(
            ok ? ConfigureState::DONE : ConfigureState::FAILED);
        if (!ok && cur.configure_attempts < 0xFF) cur.configure_attempts++;
        zigbee_pool_with_device(ieee_cp,
            [](ZapDevice* d, void* ctx) {
                const ZapDevice* src = static_cast<const ZapDevice*>(ctx);
                d->configure_state    = src->configure_state;
                d->configure_attempts = src->configure_attempts;
            }, &cur);
        zap_store_mark_dirty(&cur, ZAP_PERSIST_LOW);
    }
    reply_ok_or_err(fd, id, ok, "no def or transport down");
}

// ── device.list ──────────────────────────────────────────────────────
//
// JSON array of device summaries — the main devices page. One small
// JSON doc per row; ws_server_reply takes a single buffer so we batch
// into one ~8 KB buffer rather than chunked. For 20-30 devices this
// stays well under cap. Shadow attrs are skipped here — the SPA pulls
// detail via device.get when the user opens a row.
static void cmd_device_list(int fd, uint32_t id) {
    char* buf = (char*)heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) { send_err(fd, id, "oom"); return; }
    int pos = snprintf(buf, 8 * 1024,
                        "{\"id\":%" PRIu32 ",\"ok\":true,\"data\":[", id);
    bool first = true;

    zigbee_pool_lock();
    ZapDevice* pool = pool_all();
    const uint16_t cnt = pool_count();
    for (uint16_t i = 0; i < cnt; i++) {
        const ZapDevice& d = pool[i];
        if (zap_dev_is_removed(&d)) continue;
        // Friendly labels from the matched definition ("Tuya" / "TS0203"),
        // falling back to the device's own Basic strings when no def matches.
        // The SPA's device table reads `vendor`, NOT `manufacturer` — omitting
        // it left the Manufacturer column showing "—" for every device even
        // though the raw string was right there in the row. `manufacturer`
        // stays as the raw Basic 0x0004 value, which the detail tab reads.
        // Same contract as hap_json's device_list encoder.
        char vendor_buf[32] = {};
        char model_buf[32]  = {};
        zhac_adapter_resolve_labels(d.model_id, d.manufacturer_name,
                                    vendor_buf, sizeof(vendor_buf),
                                    model_buf,  sizeof(model_buf));
        const char* vendor_out = vendor_buf[0] ? vendor_buf : d.manufacturer_name;
        const char* model_out   = model_buf[0]  ? model_buf  : d.model_id;

        char row[512];
        int rn = snprintf(row, sizeof(row),
            "%s{\"ieee\":\"0x%016" PRIX64 "\",\"nwk\":%u,"
            "\"friendly\":\"%s\",\"name\":\"%s\","
            "\"model\":\"%s\",\"manufacturer\":\"%s\","
            "\"vendor\":\"%s\",\"model_id\":\"%s\",\"known\":%s,"
            "\"last_seen\":%" PRId64 ",\"lqi\":%u,\"battery\":%d,"
            "\"ep_count\":%u}",
            first ? "" : ",",
            d.ieee_addr, d.nwk_addr,
            d.friendly_name, d.friendly_name, model_out, d.manufacturer_name,
            vendor_out, d.model_id, model_buf[0] ? "true" : "false",
            (int64_t)d.last_seen, d.link_quality, d.battery_pct,
            d.endpoint_count);
        if (rn <= 0 || pos + rn + 4 >= 8 * 1024) break;  // truncate gracefully
        std::memcpy(buf + pos, row, rn);
        pos += rn;
        first = false;
    }
    zigbee_pool_unlock();

    if (pos + 3 < 8 * 1024) { buf[pos++] = ']'; buf[pos++] = '}'; }
    ws_server_reply(fd, buf, pos);
    heap_caps_free(buf);
}

// ── device.get ───────────────────────────────────────────────────────
//
// Full per-device snapshot for the DeviceDetail page. Includes
// endpoints, clusters, shadow attrs, and (when a def matches) exposes
// — same shape the SPA expects from net-core's `device.get` so the
// existing UI components render unchanged.
static void cmd_device_get(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    if (!ieee_s) { send_err(fd, id, "missing ieee"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }

    char* buf = (char*)heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) { send_err(fd, id, "oom"); return; }

    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) {
        zigbee_pool_unlock();
        heap_caps_free(buf);
        send_err(fd, id, "device not found");
        return;
    }

    JsonDocument out;
    out["id"]          = id;
    out["ok"]          = true;
    JsonObject D       = out["data"].to<JsonObject>();
    char ieee_buf[20];
    snprintf(ieee_buf, sizeof(ieee_buf), "0x%016" PRIX64, dev->ieee_addr);
    D["ieee"]        = ieee_buf;
    D["nwk"]         = dev->nwk_addr;
    D["friendly"]    = dev->friendly_name;
    D["name"]        = dev->friendly_name;    // net-core SPA prefers `name`
    // `manufacturer` / `model_id` are the device's raw Basic strings;
    // `vendor` / `model` are the matched definition's friendly labels with the
    // raw values as fallback. The SPA reads different ones in different views
    // (list: vendor, detail: manufacturer), so both must be present.
    char vendor_buf[32] = {};
    char model_buf[32]  = {};
    zhac_adapter_resolve_labels(dev->model_id, dev->manufacturer_name,
                                vendor_buf, sizeof(vendor_buf),
                                model_buf,  sizeof(model_buf));
    D["model"]       = model_buf[0] ? (const char*)model_buf
                                    : (const char*)dev->model_id;
    D["vendor"]      = vendor_buf[0] ? (const char*)vendor_buf
                                     : (const char*)dev->manufacturer_name;
    D["manufacturer"]= dev->manufacturer_name;
    D["model_id"]    = dev->model_id;
    D["mfr"]         = dev->manufacturer_code;
    D["power_source"]= dev->power_source;
    D["type"]        = dev->device_type;
    D["last_seen"]   = (int64_t)dev->last_seen;
    D["lqi"]         = dev->link_quality;
    D["bat_pct"]     = dev->battery_pct;
    D["ep_count"]    = dev->endpoint_count;

    JsonArray eps = D["eps"].to<JsonArray>();
    for (uint8_t i = 0; i < dev->endpoint_count && i < 8; i++) {
        eps.add(dev->endpoints[i]);
    }
    JsonArray cls = D["clusters"].to<JsonArray>();
    for (uint8_t i = 0; i < dev->endpoint_count && i < 8; i++) {
        JsonArray row = cls.add<JsonArray>();
        for (uint8_t j = 0; j < ZAP_CLUSTERS_PER_EP; j++) {
            if (dev->clusters[i][j] != 0) row.add(dev->clusters[i][j]);
        }
    }

    ShadowAttr sa[32];
    uint8_t nsa = device_shadow_get_attrs(dev->ieee_addr, sa, 32);
    JsonObject attrs = D["attrs"].to<JsonObject>();
    for (uint8_t j = 0; j < nsa; j++) {
        if (sa[j].key[0] == '_') continue;   // filter internal keys
        switch (sa[j].val_type) {
            case VAL_INT:
            case VAL_BOOL: attrs[sa[j].key] = sa[j].int_val; break;
            case VAL_STR:  attrs[sa[j].key] = sa[j].str_val; break;
            // Floats are stored x100; divide at the JSON boundary, as
            // hap_json does for the dual-chip build. Dropping them here
            // blanked temperature, humidity, power... on the detail page.
            case VAL_FLOAT: attrs[sa[j].key] = static_cast<float>(sa[j].int_val) / 100.0f; break;
            default: break;
        }
    }

    // Snapshot model/manuf strings — exposes builder reads them outside the lock.
    // Sized to exceed ZapDevice::{model_id, manufacturer_name} (~34 B
    // each); 32-byte dest trips -Werror=format-truncation.
    char model_cp[64], manu_cp[64];
    snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
    snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
    const uint64_t ieee_cp = dev->ieee_addr;
    zigbee_pool_unlock();

    char exposes_json[2048];
    size_t exp_n = zhac_adapter_build_exposes_json(
        ieee_cp, model_cp, manu_cp, exposes_json, sizeof(exposes_json));
    if (exp_n > 0) {
        // Parse the array string back so ArduinoJson includes it as a
        // JsonArray rather than a quoted string.
        JsonDocument exp_doc;
        if (deserializeJson(exp_doc, exposes_json, exp_n) ==
            DeserializationError::Ok) {
            D["exposes"] = exp_doc.as<JsonArray>();
        }
    }

    size_t n = serializeJson(out, buf, 8 * 1024);
    ws_server_reply(fd, buf, n);
    heap_caps_free(buf);
}

// ── device.rename ────────────────────────────────────────────────────
//
// Sets `friendly_name` in the pool, marks NVS dirty so the next flush
// persists. Empty name is rejected — the SPA enforces a non-empty
// trim, but defend at the boundary too.
static void cmd_device_rename(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    const char* name   = doc["args"]["name"] | (const char*)nullptr;
    if (!ieee_s) { send_err(fd, id, "missing ieee"); return; }
    if (!name || !name[0]) { send_err(fd, id, "missing name"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }
    const DevCmdResult r = device_cmd_rename(ieee, name);   // validates, persists, reloads rules
    reply_ok_or_err(fd, id, r == DEVCMD_OK, device_cmd_result_str(r));
}

// ── device.delete ────────────────────────────────
//
// One contract for every core, owned by device_cmd_remove: soft asks the
// device to leave and tombstones it (hidden from lists, name kept for a
// rejoin); `args.hard = true` also wipes the pool slot, shadow, converter
// caches and the stored row.
static void cmd_device_delete(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    const bool  hard   = doc["args"]["hard"] | false;
    if (!ieee_s) { send_err(fd, id, "missing ieee"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }
    const DevCmdResult r = device_cmd_remove(ieee, hard);
    reply_ok_or_err(fd, id, r == DEVCMD_OK, device_cmd_result_str(r));
}

// ── device.attr.set ──────────────────────────────────────────────────
//
// Dispatches by JSON value type:
//   bool   → zhac_adapter_send_bool
//   uint   → zhac_adapter_send_uint
//   string → zhac_adapter_send_string
//
// Mirrors net-core's `api_device_attr_set`. Keys live in the
// PreparedDefinition's `to_zigbee[]` table; unknown keys return
// "no zhc converter".
// Write one attribute through the device's converter. Shared by the WS
// command below and Home Assistant commands arriving over MQTT (ha_glue.cpp).
// On failure `*err` names the reason in the same words the WS reply uses.
bool ws_bridge_attr_set(uint64_t ieee, const char* key, JsonVariantConst v,
                        const char** err) {
    const char* e = nullptr;
    const char** err_out = err ? err : &e;
    DevCmdValue val;
    if (!device_cmd_value_from_json(v, &val)) {
        *err_out = device_cmd_result_str(DEVCMD_BAD_VALUE);
        return false;
    }
    const DevCmdResult r = device_cmd_set_attr(ieee, 0, key, &val);
    if (r != DEVCMD_OK) *err_out = device_cmd_result_str(r);
    return r == DEVCMD_OK;
}

static void cmd_device_attr_set(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    const char* key    = doc["args"]["key"]  | (const char*)nullptr;
    if (!ieee_s || !key) { send_err(fd, id, "missing ieee or key"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }
    const char* err = nullptr;
    if (ws_bridge_attr_set(ieee, key, doc["args"]["value"], &err)) {
        reply_ok_or_err(fd, id, true, nullptr);
    } else {
        send_err(fd, id, err ? err : "failed");
    }
}
// ── device.options.set ────────────────────────────────────────────────
//
// args {ieee, occupancy_timeout?, debounce_ms?, flood_protection?,
// throttle_ms?}. Applied directly to device_shadow + persisted to NVS.
// Mirrors net-core's `api_device_options_set` minus the HAP hop.
static void cmd_device_options_set(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"] | (const char*)nullptr;
    if (!ieee_s) { send_err(fd, id, "missing ieee"); return; }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }

    char args[192];
    size_t n = serializeJson(doc["args"], args, sizeof(args));
    bool ok = (n > 0 && n < sizeof(args)) &&
              device_options_set(ieee, args, n);
    reply_ok_or_err(fd, id, ok, "options apply failed");
}

// ── device.bind ──────────────────────────────────────────────────────
//
// SPA's BindTab calls this with `{ ieee, src_ep, cluster, unbind?:bool }`.
// Destination defaults to the coordinator (`zigbee_mgr_coordinator_ieee()`
// on EP 1). Unbind variant sends ZDO Unbind_req (cmd 0x22) instead of
// Bind_req (cmd 0x21).
static void cmd_device_bind(int fd, uint32_t id, JsonDocument& doc) {
    const char* ieee_s = doc["args"]["ieee"]    | (const char*)nullptr;
    const int   src_ep = doc["args"]["src_ep"]  | -1;
    const int   cl     = doc["args"]["cluster"] | -1;
    const bool  unbind = doc["args"]["unbind"]  | false;
    if (!ieee_s || src_ep < 0 || cl < 0) {
        send_err(fd, id, "missing ieee / src_ep / cluster"); return;
    }
    uint64_t ieee = parse_ieee(ieee_s);
    if (ieee == 0) { send_err(fd, id, "bad ieee"); return; }
    const uint64_t coord = zigbee_mgr_coordinator_ieee();
    if (coord == 0) { send_err(fd, id, "coordinator ieee unknown"); return; }

    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) {
        zigbee_pool_unlock();
        send_err(fd, id, "device not found");
        return;
    }
    const uint16_t nwk_cp = dev->nwk_addr;
    zigbee_pool_unlock();

    const bool ok = unbind
        ? zigbee_zdo_unbind(nwk_cp, ieee, (uint8_t)src_ep, (uint16_t)cl,
                             coord, /*dst_ep=*/1)
        : zigbee_zdo_bind  (nwk_cp, ieee, (uint8_t)src_ep, (uint16_t)cl,
                             coord, /*dst_ep=*/1);
    reply_ok_or_err(fd, id, ok, unbind ? "unbind failed" : "bind failed");
}

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
static void cmd_remote_status(int fd, uint32_t id) {
    char tmp[256]; size_t tn = remote_status_json(tmp, sizeof(tmp));
    char buf[320];
    int n = (tn >= 2)
        ? snprintf(buf, sizeof(buf), "{\"id\":%u,\"ok\":true,\"data\":%.*s}", (unsigned)id, (int)tn, tmp)
        : snprintf(buf, sizeof(buf), "{\"id\":%u,\"ok\":true,\"data\":{}}", (unsigned)id);
    ws_server_reply(fd, buf, (size_t)n);
}
static void cmd_remote_connect(int fd, uint32_t id, JsonDocument& doc) {
    char args[320]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    reply_ok_or_err(fd, id, remote_connect_req(args, an), "url + token required");
}
static void cmd_remote_disconnect(int fd, uint32_t id, JsonDocument& doc) {
    char args[64]; size_t an = serializeJson(doc["args"], args, sizeof(args));
    bool forget = false; remote_disconnect_req(args, an, &forget);
    reply_ok_or_err(fd, id, true, nullptr);
}
#endif

// ── device.groups.* / groups.all — native ZCL group membership ────────
//
// NOT the Collections feature above. A device in ZCL group N obeys commands
// addressed to N, including groupcasts a hardware zone-remote sends directly —
// so the remote keeps driving the bulbs while the gateway is rebooting.
// Collections are gateway fan-out; these are the device's own group table.
//
// dgm_store is a MIRROR of what we provisioned, kept so the UI can list
// membership without waking every device. The device's table is authoritative,
// which is what `refresh` reconciles against.
//
// Unlike net-core, which reaches the radio over a HAP roundtrip to the P4,
// this SKU calls the backend in-process.
static uint64_t dgm_arg_ieee(JsonDocument& doc) {
    const char* s = doc["args"]["ieee"] | (const char*)nullptr;
    return s ? parse_ieee(s) : 0;
}

// Reply shape the SPA expects: {"groups":[101,102]}.
static void dgm_reply_list(int fd, uint32_t id, uint64_t ieee) {
    uint16_t gids[DGM_MAX_GIDS];
    const uint8_t n = dgm_list(ieee, gids, DGM_MAX_GIDS);
    char buf[256];
    int p = snprintf(buf, sizeof(buf),
                     "{\"id\":%" PRIu32 ",\"ok\":true,\"data\":{\"groups\":[", id);
    for (uint8_t i = 0; i < n && p > 0 && (size_t)p < sizeof(buf); i++)
        p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s%u", i ? "," : "", gids[i]);
    if (p > 0 && (size_t)p < sizeof(buf))
        p += snprintf(buf + p, sizeof(buf) - (size_t)p, "]}}");
    ws_server_reply(fd, buf, p);
}

static void cmd_device_groups_list(int fd, uint32_t id, JsonDocument& doc) {
    const uint64_t ieee = dgm_arg_ieee(doc);
    if (ieee == 0) { send_err(fd, id, "missing or bad ieee"); return; }
    dgm_reply_list(fd, id, ieee);
}

static void cmd_device_groups_add_remove(int fd, uint32_t id, JsonDocument& doc,
                                         bool remove) {
    const uint64_t ieee = dgm_arg_ieee(doc);
    if (ieee == 0) { send_err(fd, id, "missing or bad ieee"); return; }
    uint16_t gid = doc["args"]["gid"] | (uint16_t)0;
    if (gid == 0) gid = doc["args"]["group"] | (uint16_t)0;
    const uint8_t ep = doc["args"]["ep"] | (uint8_t)1;
    if (gid == 0) { send_err(fd, id, "gid 1-65535"); return; }

    ZapDevice snap{};
    if (!zigbee_pool_snapshot(ieee, &snap) || snap.nwk_addr == 0) {
        send_err(fd, id, "device not found");
        return;
    }
    const bool ok = remove ? zigbee_zcl_group_remove(snap.nwk_addr, ep, gid)
                           : zigbee_zcl_group_add(snap.nwk_addr, ep, gid);
    if (!ok) { send_err(fd, id, "command failed"); return; }
    // Mirror only after the radio accepted it, so a failed send cannot leave
    // the UI claiming a membership the device never got.
    if (remove) dgm_remove(ieee, gid); else dgm_add(ieee, gid);
    dgm_reply_list(fd, id, ieee);
}

// Read the device's ACTUAL table and reconcile the mirror to it.
static void cmd_device_groups_refresh(int fd, uint32_t id, JsonDocument& doc) {
    const uint64_t ieee = dgm_arg_ieee(doc);
    if (ieee == 0) { send_err(fd, id, "missing or bad ieee"); return; }
    const uint8_t ep = doc["args"]["ep"] | (uint8_t)1;

    ZapDevice snap{};
    if (!zigbee_pool_snapshot(ieee, &snap) || snap.nwk_addr == 0) {
        send_err(fd, id, "device not found");
        return;
    }
    uint16_t gids[DGM_MAX_GIDS];
    uint8_t  count = 0;
    if (!zigbee_zcl_get_group_membership(snap.nwk_addr, ep, gids,
                                         DGM_MAX_GIDS, &count)) {
        // No answer is NOT "device is in no groups". Reconciling to empty here
        // would wipe a working configuration because a sleepy device missed
        // its window, so the mirror is left exactly as it was.
        send_err(fd, id, "device did not answer");
        return;
    }
    dgm_set(ieee, gids, count);
    dgm_reply_list(fd, id, ieee);
}

// Whole-store view, inverted by gid — backs the SPA's global Groups tab.
// PSRAM buffer: the response scales with every device * every group, well past
// what belongs on the httpd task's stack.
static void cmd_groups_all(int fd, uint32_t id) {
    constexpr size_t CAP = 8192;
    char* tmp = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    char* buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!tmp || !buf) { heap_caps_free(tmp); heap_caps_free(buf); send_err(fd, id, "oom"); return; }
    const size_t tn = dgm_all_json_live(tmp, CAP);
    JsonDocument inner;
    // n == 0 means the serializer overflowed or the store is unreadable; send a
    // well-formed empty result rather than a truncated body the SPA cannot parse.
    if (tn == 0 || deserializeJson(inner, tmp, tn)) {
        inner.clear();
        inner["groups"].to<JsonArray>();
    }
    JsonDocument env; env["id"] = id; env["ok"] = true; env["data"] = inner;
    const size_t n = serializeJson(env, buf, CAP);
    ws_server_reply(fd, buf, n);
    heap_caps_free(tmp); heap_caps_free(buf);
}

// ── uplink.get ────────────────────────────────────────────────────────
//
// The SPA asks this before rendering a device's Options tab. Without it the
// call rejects and the tab shows "Couldn't load RainMaker status" with a Retry
// button -- on every device, forever, on a SKU that has no RainMaker by design
// (tools/check_sku_invariants.sh makes that a build failure, not a convention).
//
// Answering "none" is the truthful answer and makes the SPA render its correct
// "RainMaker bridging is off" copy instead of an error. There is deliberately
// no uplink.set: there is nothing here to switch to.
// ota.update {url} — see ota_update.h. Replies at once; progress follows as
// ota.start / ota.progress / ota.complete events (target "self").
static void cmd_ota_update(int fd, uint32_t id, JsonDocument& doc) {
    const char* url = doc["args"]["url"] | (const char*)nullptr;
    const char* err = nullptr;
    if (ota_update_start(url, &err)) reply_ok_or_err(fd, id, true, nullptr);
    else                             send_err(fd, id, err ? err : "failed");
}

static void cmd_uplink_get(int fd, uint32_t id) {
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%" PRIu32 ",\"ok\":true,"
                     "\"data\":{\"uplink\":\"none\"}}", id);
    ws_server_reply(fd, buf, n);
}

// ── zigbee.permit_join / .status ──────────────────────────────────────
//
// The SPA drives pairing over WS, not REST: its Devices toolbar calls
// `zigbee.permit_join` and then polls `zigbee.permit_join.status` for the
// countdown badge. Both were missing here, so the button failed with
// "unknown cmd" while the REST endpoint it never calls worked fine.
//
// Duration is range-checked against the Zigbee spec (0-254); 0 closes the
// window. device_cmd owns the deadline so REST and WS agree.
static void cmd_zigbee_permit_join(int fd, uint32_t id, JsonDocument& doc) {
    const int duration = doc["args"]["duration"] | -1;
    if (duration < 0 || duration > 254) { send_err(fd, id, "duration 0-254"); return; }
    const DevCmdResult r = device_cmd_permit_join((uint8_t)duration);
    reply_ok_or_err(fd, id, r == DEVCMD_OK, "permit_join failed");
}

static void cmd_zigbee_permit_join_status(int fd, uint32_t id) {
    bool open = false;
    int  remaining = 0;
    device_cmd_permit_join_status(&open, &remaining);
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%" PRIu32 ",\"ok\":true,\"data\":{"
                     "\"open\":%s,\"remaining_sec\":%d}}",
                     id, open ? "true" : "false", remaining);
    ws_server_reply(fd, buf, n);
}

// ── push helper: {event, data} broadcast (+ relay mirror) ─────────────
void ws_push(const char* event, JsonDocument& data) {
    if (ws_server_client_count() == 0) return;
    JsonDocument env; env["event"] = event; env["data"] = data;
    // 1024, not 512: the status.tick payload alone serialises to ~500 bytes
    // and the envelope adds more. serializeJson TRUNCATES rather than failing,
    // so an undersized buffer broadcasts malformed JSON that every client
    // silently drops -- a push that looks sent and never arrives.
    char buf[1024];
    const size_t cap = sizeof(buf);
    const size_t n = serializeJson(env, buf, cap);
    if (n == 0 || n >= cap) {
        ESP_LOGW(TAG, "ws_push('%s') dropped: payload %u B exceeds %u B buffer",
                 event, (unsigned)measureJson(env), (unsigned)cap);
        return;
    }
    ws_server_broadcast(buf, n);
    remote_client_publish_event(event, buf, n);
}

// ── rule.* (SPA drives rules over WS; calls simple_rules + rule_store) ──
static void cmd_rule_list(int fd, uint32_t id) {
    auto* slots = (RuleSlot*)heap_caps_malloc(sizeof(RuleSlot) * ZAP_MAX_RULES, MALLOC_CAP_SPIRAM);
    char* buf   = (char*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    if (!slots || !buf) { heap_caps_free(slots); heap_caps_free(buf); send_err(fd, id, "oom"); return; }
    uint16_t cnt = rule_store_load_all(slots, ZAP_MAX_RULES);
    int pos = snprintf(buf, 16 * 1024, "{\"id\":%u,\"ok\":true,\"data\":[", (unsigned)id);
    for (uint16_t i = 0; i < cnt; i++) {
        const RuleSlot& s = slots[i];
        JsonDocument o;
        o["id"] = s.rule_id; o["enabled"] = (bool)s.enabled;
        o["trigger_type"] = s.trigger_type; o["rule_type"] = s.rule_type; o["name"] = s.name;
        char dsl[501]; size_t dn = s.src_len < 500 ? s.src_len : 500;
        memcpy(dsl, s.src, dn); dsl[dn] = '\0'; o["dsl"] = dsl;
        char row[768]; size_t rn = serializeJson(o, row, sizeof(row));
        if (pos + (int)rn + 4 >= 16 * 1024) break;
        if (i) buf[pos++] = ',';
        memcpy(buf + pos, row, rn); pos += rn;
    }
    pos += snprintf(buf + pos, 16 * 1024 - pos, "]}");
    ws_server_reply(fd, buf, pos);
    heap_caps_free(slots); heap_caps_free(buf);
}
static void cmd_rule_create(int fd, uint32_t id, JsonDocument& doc) {
    const char* name = doc["args"]["name"] | "";
    const char* dsl  = doc["args"]["dsl"]  | (const char*)nullptr;
    if (!dsl || !dsl[0]) { send_err(fd, id, "missing dsl"); return; }
    uint16_t nid = 0;
    if (!simple_rules_add(name, dsl, &nid)) { send_err(fd, id, dsl_last_error()); return; }
    reply_ok_or_err(fd, id, true, nullptr);
    JsonDocument p; p["id"] = nid; p["name"] = name; p["dsl"] = dsl; p["enabled"] = true;
    ws_push("rule.added", p);
}
static void cmd_rule_update(int fd, uint32_t id, JsonDocument& doc) {
    uint16_t rid = doc["args"]["id"] | (uint16_t)0;
    const char* name = doc["args"]["name"] | "";
    const char* dsl  = doc["args"]["dsl"]  | (const char*)nullptr;
    if (!rid || !dsl) { send_err(fd, id, "missing id/dsl"); return; }
    if (!simple_rules_update(rid, name, dsl)) { send_err(fd, id, dsl_last_error()); return; }
    reply_ok_or_err(fd, id, true, nullptr);
    JsonDocument p; p["id"] = rid; p["name"] = name; p["dsl"] = dsl;
    ws_push("rule.updated", p);
}
static void cmd_rule_enable(int fd, uint32_t id, JsonDocument& doc) {
    uint16_t rid = doc["args"]["id"] | (uint16_t)0;
    bool en = doc["args"]["enabled"] | false;
    if (!rid) { send_err(fd, id, "missing id"); return; }
    bool ok = simple_rules_enable(rid, en);
    reply_ok_or_err(fd, id, ok, "rule not found");
    if (ok) { JsonDocument p; p["id"] = rid; p["enabled"] = en; ws_push("rule.updated", p); }
}
static void cmd_rule_delete(int fd, uint32_t id, JsonDocument& doc) {
    uint16_t rid = doc["args"]["id"] | (uint16_t)0;
    if (!rid) { send_err(fd, id, "missing id"); return; }
    bool ok = simple_rules_delete(rid);
    reply_ok_or_err(fd, id, ok, "rule not found");
    if (ok) { JsonDocument p; p["id"] = rid; ws_push("rule.deleted", p); }
}

// ── script.* (SPA drives scripts over WS; write stays REST) ────────────
static void cmd_script_list(int fd, uint32_t id) {
    LuaScriptEntry list[LUA_SCRIPT_MAX];
    uint16_t n = lua_script_cache_list(list, LUA_SCRIPT_MAX);
    JsonDocument d; d["id"] = id; d["ok"] = true;
    JsonArray arr = d["data"].to<JsonArray>();
    for (uint16_t i = 0; i < n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = list[i].name; o["size"] = list[i].size;
    }
    char buf[1024]; size_t bn = serializeJson(d, buf, sizeof(buf)); ws_server_reply(fd, buf, bn);
}
static void cmd_script_read(int fd, uint32_t id, JsonDocument& doc) {
    const char* name = doc["args"]["name"] | (const char*)nullptr;
    if (!name) { send_err(fd, id, "missing name"); return; }
    char* src = (char*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    char* buf = (char*)heap_caps_malloc(20 * 1024, MALLOC_CAP_SPIRAM);
    if (!src || !buf) { heap_caps_free(src); heap_caps_free(buf); send_err(fd, id, "oom"); return; }
    int sn = lua_script_cache_read(name, src, 16 * 1024);
    if (sn < 0) { heap_caps_free(src); heap_caps_free(buf); send_err(fd, id, "not found"); return; }
    src[sn < 16 * 1024 ? sn : 16 * 1024 - 1] = '\0';
    JsonDocument d; d["id"] = id; d["ok"] = true;
    d["data"]["src"] = (const char*)src;   // const* → ArduinoJson references (no 16K copy)
    size_t bn = serializeJson(d, buf, 20 * 1024);
    ws_server_reply(fd, buf, bn);
    heap_caps_free(src); heap_caps_free(buf);
}
static void cmd_script_delete(int fd, uint32_t id, JsonDocument& doc) {
    const char* name = doc["args"]["name"] | (const char*)nullptr;
    if (!name) { send_err(fd, id, "missing name"); return; }
    bool ok = lua_script_cache_delete(name);
    reply_ok_or_err(fd, id, ok, "not found");
    if (ok) { JsonDocument p; p["name"] = name; ws_push("script.deleted", p); }
}
static void cmd_script_run(int fd, uint32_t id, JsonDocument& doc) {
    const char* name = doc["args"]["name"] | (const char*)nullptr;
    if (!name) { send_err(fd, id, "missing name"); return; }
    reply_ok_or_err(fd, id, lua_engine_run_script(name), "queue saturated or missing");
}
static void cmd_script_check(int fd, uint32_t id, JsonDocument& doc) {
    const char* name = doc["args"]["name"] | "";
    const char* src  = doc["args"]["src"]  | (const char*)nullptr;
    if (!src) { send_err(fd, id, "missing src"); return; }
    char err[128] = {0}; int line = 0;
    bool ok = lua_engine_check_syntax(name, src, err, sizeof(err), &line);
    JsonDocument d; d["id"] = id; d["ok"] = true;
    JsonObject da = d["data"].to<JsonObject>();
    da["ok"] = ok; da["err"] = err; da["line"] = line;
    char buf[256]; size_t bn = serializeJson(d, buf, sizeof(buf)); ws_server_reply(fd, buf, bn);
}

// ── alerts.get ────────────────────────────────────────────────────────
// No local alert producer yet (see WS_PROTOCOL.md / PARITY_AUDIT.md); return
// an empty array so the SPA alerts page renders instead of erroring on an
// unknown cmd. Wire to a real alert ring once a local producer exists.
static void cmd_alerts_get(int fd, uint32_t id) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"id\":%u,\"ok\":true,\"data\":[]}", (unsigned)id);
    ws_server_reply(fd, buf, (size_t)n);
}

// ── zigbee.* (config persist + factory reset) ─────────────────────────
static void cmd_zigbee_settings_set(int fd, uint32_t id, JsonDocument& doc) {
    nvs_handle_t h;
    if (nvs_open("zigbee_cfg", NVS_READWRITE, &h) != ESP_OK) { send_err(fd, id, "nvs"); return; }
    if (doc["args"]["channel"].is<int>())
        nvs_set_u8(h, "channel", (uint8_t)doc["args"]["channel"].as<int>());
    if (doc["args"]["regenerate"] | false) {
        uint8_t k[16]; esp_fill_random(k, sizeof(k)); nvs_set_blob(h, "net_key", k, sizeof(k));
    } else if (doc["args"]["net_key_hex"].is<const char*>()) {
        const char* hx = doc["args"]["net_key_hex"];
        if (std::strlen(hx) >= 32) {
            uint8_t k[16];
            for (int i = 0; i < 16; i++) {
                char b[3] = {hx[i * 2], hx[i * 2 + 1], 0};
                k[i] = (uint8_t)strtoul(b, nullptr, 16);
            }
            nvs_set_blob(h, "net_key", k, sizeof(k));
        }
    }
    bool ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    reply_ok_or_err(fd, id, ok, "nvs commit");   // applies on next zigbee.reset
}
// Erase the hub's storage on the owner's explicit request -- the boot path
// never does it by itself any more (see main.cpp). Gated like every command.
static void cmd_storage_reset(int fd, uint32_t id) {
    ESP_LOGW(TAG, "storage reset requested -- erasing NVS and restarting");
    const esp_err_t e = nvs_flash_erase();
    reply_ok_or_err(fd, id, e == ESP_OK, esp_err_to_name(e));
    if (e != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void cmd_zigbee_reset(int fd, uint32_t id) {
    zigbee_force_recommission();   // wipe ZNP marker → fresh BDB network on reboot
    reply_ok_or_err(fd, id, true, nullptr);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Dispatch a parsed command envelope. Shared by the local WS rx path and,
// when remote is enabled, the relay path via dispatch_envelope_for_remote().
static void dispatch_envelope(int fd, JsonDocument& doc) {
    uint32_t    id  = doc["id"]  | (uint32_t)0;
    const char* cmd = doc["cmd"] | (const char*)nullptr;
    if (!cmd) { send_err(fd, id, "missing cmd"); return; }

    // With auth on, a browser socket must send {"cmd":"auth","args":{"token":
    // ...}} before anything else runs -- the token rides a frame, never the
    // URL (as on the dual-chip S3). fd < 0 is the remote relay, which has its
    // own authentication.
    if (auth_enabled() && fd >= 0 && !ws_server_fd_is_authed(fd)) {
        const bool is_auth = std::strcmp(cmd, "auth") == 0;
        if (is_auth && auth_check_token(doc["args"]["token"] | "")) {
            ws_server_fd_set_authed(fd);
            reply_ok_or_err(fd, id, true, nullptr);
        } else {
            send_err(fd, id, is_auth ? "auth failed" : "auth required");
        }
        return;
    }
    if (std::strcmp(cmd, "auth")               == 0) { reply_ok_or_err(fd, id, true, nullptr); return; }
    if (std::strcmp(cmd, "ping")               == 0) { cmd_ping(fd, id);   return; }
    if (std::strcmp(cmd, "status")             == 0) { cmd_status(fd, id); return; }
    if (std::strcmp(cmd, "status.get")         == 0) { cmd_status(fd, id); return; }
    if (std::strcmp(cmd, "time.set")           == 0) { cmd_time_set(fd, id, doc); return; }
    if (std::strcmp(cmd, "device.list")        == 0) { cmd_device_list(fd, id);              return; }
    if (std::strcmp(cmd, "device.get")         == 0) { cmd_device_get(fd, id, doc);          return; }
    if (std::strcmp(cmd, "device.rename")      == 0) { cmd_device_rename(fd, id, doc);       return; }
    if (std::strcmp(cmd, "device.delete")      == 0) { cmd_device_delete(fd, id, doc);       return; }
    if (std::strcmp(cmd, "device.attr.set")    == 0) { cmd_device_attr_set(fd, id, doc);     return; }
    if (std::strcmp(cmd, "device.bind")        == 0) { cmd_device_bind(fd, id, doc);         return; }
    if (std::strcmp(cmd, "device.options.set") == 0) { cmd_device_options_set(fd, id, doc);  return; }
    if (std::strcmp(cmd, "settings.set")          == 0) { cmd_settings_set(fd, id, doc);          return; }
    if (std::strcmp(cmd, "token.rotate")          == 0) { cmd_token_rotate(fd, id);               return; }
    if (std::strcmp(cmd, "diagnostics.unhandled")     == 0) { cmd_diagnostics_unhandled(fd, id);   return; }
    if (std::strcmp(cmd, "diagnostics.unhandled.get") == 0) { cmd_diagnostics_unhandled(fd, id);   return; }
    if (std::strcmp(cmd, "alerts.get")            == 0) { cmd_alerts_get(fd, id);                 return; }
    if (std::strcmp(cmd, "group.list")            == 0) { cmd_group_list(fd, id);                  return; }
    if (std::strcmp(cmd, "group.create")          == 0) { cmd_group_create(fd, id, doc);           return; }
    if (std::strcmp(cmd, "group.get")             == 0) { cmd_group_get(fd, id, doc);              return; }
    if (std::strcmp(cmd, "group.update")          == 0) { cmd_group_update(fd, id, doc);           return; }
    if (std::strcmp(cmd, "group.delete")          == 0) { cmd_group_delete(fd, id, doc);           return; }
    if (std::strcmp(cmd, "group.cmd")             == 0) { cmd_group_cmd(fd, id, doc);              return; }
    if (std::strcmp(cmd, "net.status")            == 0) { cmd_net_status(fd, id);                  return; }
    // wifi.* kept registered for the shared SPA; see cmd_net_status above.
    if (std::strcmp(cmd, "wifi.status")           == 0) { cmd_wifi_status(fd, id);                 return; }
    if (std::strcmp(cmd, "wifi.scan")             == 0) { cmd_wifi_scan(fd, id);                   return; }
    if (std::strcmp(cmd, "wifi.connect")          == 0) { cmd_wifi_unsupported(fd, id);            return; }
    if (std::strcmp(cmd, "wifi.disconnect")       == 0) { cmd_wifi_unsupported(fd, id);            return; }
    if (std::strcmp(cmd, "logs.get")              == 0) { cmd_logs_get(fd, id);                    return; }
    if (std::strcmp(cmd, "device.reinterview") == 0) { cmd_device_reinterview(fd, id, doc);  return; }
    if (std::strcmp(cmd, "device.configure")   == 0) { cmd_device_configure(fd, id, doc);    return; }
    if (std::strcmp(cmd, "rule.list")          == 0) { cmd_rule_list(fd, id);                return; }
    if (std::strcmp(cmd, "rule.create")        == 0) { cmd_rule_create(fd, id, doc);         return; }
    if (std::strcmp(cmd, "rule.update")        == 0) { cmd_rule_update(fd, id, doc);         return; }
    if (std::strcmp(cmd, "rule.enable")        == 0) { cmd_rule_enable(fd, id, doc);         return; }
    if (std::strcmp(cmd, "rule.delete")        == 0) { cmd_rule_delete(fd, id, doc);         return; }
    if (std::strcmp(cmd, "script.list")        == 0) { cmd_script_list(fd, id);              return; }
    if (std::strcmp(cmd, "script.read")        == 0) { cmd_script_read(fd, id, doc);         return; }
    if (std::strcmp(cmd, "script.delete")      == 0) { cmd_script_delete(fd, id, doc);       return; }
    if (std::strcmp(cmd, "script.run")         == 0) { cmd_script_run(fd, id, doc);          return; }
    if (std::strcmp(cmd, "script.check")       == 0) { cmd_script_check(fd, id, doc);        return; }
    if (std::strcmp(cmd, "zigbee.settings.set") == 0) { cmd_zigbee_settings_set(fd, id, doc); return; }
    if (std::strcmp(cmd, "zigbee.reset")        == 0) { cmd_zigbee_reset(fd, id);             return; }
    if (std::strcmp(cmd, "system.storage_reset") == 0) { cmd_storage_reset(fd, id);           return; }
    if (std::strcmp(cmd, "zigbee.permit_join")  == 0) { cmd_zigbee_permit_join(fd, id, doc);  return; }
    if (std::strcmp(cmd, "zigbee.permit_join.status") == 0) { cmd_zigbee_permit_join_status(fd, id); return; }
    if (std::strcmp(cmd, "uplink.get")          == 0) { cmd_uplink_get(fd, id);              return; }
    if (std::strcmp(cmd, "ota.update")          == 0) { cmd_ota_update(fd, id, doc);         return; }
    if (std::strcmp(cmd, "device.groups.list")  == 0) { cmd_device_groups_list(fd, id, doc);  return; }
    if (std::strcmp(cmd, "device.groups.add")   == 0) { cmd_device_groups_add_remove(fd, id, doc, false); return; }
    if (std::strcmp(cmd, "device.groups.remove")== 0) { cmd_device_groups_add_remove(fd, id, doc, true);  return; }
    if (std::strcmp(cmd, "device.groups.refresh")==0) { cmd_device_groups_refresh(fd, id, doc); return; }
    if (std::strcmp(cmd, "groups.all")          == 0) { cmd_groups_all(fd, id);               return; }
#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    if (std::strcmp(cmd, "remote.status")      == 0) { cmd_remote_status(fd, id);            return; }
    if (std::strcmp(cmd, "remote.connect")     == 0) { cmd_remote_connect(fd, id, doc);      return; }
    if (std::strcmp(cmd, "remote.disconnect")  == 0) { cmd_remote_disconnect(fd, id, doc);   return; }
#endif
    send_err(fd, id, "unknown cmd");
}

static void ws_rx(int fd, const char* data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) { send_err(fd, 0, "bad json"); return; }
    dispatch_envelope(fd, doc);
}

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
// Relay-delivered command (fd == REMOTE_VIRTUAL_FD). The reply routes back to
// the remote send-hook registered in remote_client_init().
extern "C" void dispatch_envelope_for_remote(int fd, JsonDocument& doc) {
    dispatch_envelope(fd, doc);
}
#endif

// ── Outbound push: event_bus → ws_server_broadcast ──────────────────────
//
// Subscribers run on the event_bus dispatch task; the broadcast call
// is non-blocking (lwip_send) but acquires the ws_server mutex briefly
// to walk the client list. Cheap enough for the 1-10 events/s steady
// state we expect with 20-30 devices.
//
// `value` is rendered as integer or string depending on val_type so
// the SPA doesn't have to guess. Numeric reasonable for state/level
// and string for action attrs.

static void on_zcl_attr(const Event& e) {
    if (ws_server_client_count() == 0) return;
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    JsonDocument doc;
    doc["event"] = "attr.changed";                 // SPA push name
    JsonObject d = doc["data"].to<JsonObject>();
    char ieee_s[20];
    snprintf(ieee_s, sizeof(ieee_s), "0x%016" PRIX64, z.ieee);
    d["ieee"]     = ieee_s;
    d["key"]      = z.key;
    switch (z.val_type) {
        case VAL_INT:
        case VAL_BOOL:  d["value"] = z.int_val; break;
        case VAL_STR:   d["value"] = z.str_val; break;
        case VAL_FLOAT: d["value"] = static_cast<float>(z.int_val) / 100.0f; break;  // stored x100
        default: break;
    }
    d["nwk"]      = z.nwk;
    d["ep"]       = z.ep;
    d["cluster"]  = z.cluster;
    d["attr_id"]  = z.attr_id;
    // The web UI takes this as the device's "last seen". Left out while the
    // clock is unset, so the UI keeps the value it has.
    if (const uint32_t t = esp_zigbee_backend_wall_clock_s()) d["ts"] = t;
    char buf[288];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    ws_server_broadcast(buf, n);
    remote_client_publish_event("attr.changed", buf, n);   // mirror to relay (no-op if off)
}

static void on_device_join(const Event& e) {
    if (ws_server_client_count() == 0) return;
    // DEVICE_JOIN payload starts with ieee + friendly name; encode the
    // standard layout used by zigbee_mgr.
    struct __attribute__((packed)) JoinPayload {
        uint64_t ieee;
        char     friendly[30];
    };
    const auto& j = *reinterpret_cast<const JoinPayload*>(e.data);
    char buf[224];
    int n = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device.added\",\"data\":{\"ieee\":\"0x%016" PRIX64
                     "\",\"friendly\":\"%.*s\",\"name\":\"%.*s\"}}",
                     j.ieee, (int)sizeof(j.friendly), j.friendly,
                     (int)sizeof(j.friendly), j.friendly);
    ws_server_broadcast(buf, n);
    remote_client_publish_event("device.added", buf, (size_t)n);
}

static void on_device_leave(const Event& e) {
    if (ws_server_client_count() == 0) return;
    uint64_t ieee = 0;
    std::memcpy(&ieee, e.data, sizeof(ieee));
    char buf[112];
    int n = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device.removed\",\"data\":{\"ieee\":\"0x%016" PRIX64 "\"}}",
                     ieee);
    ws_server_broadcast(buf, n);
    remote_client_publish_event("device.removed", buf, (size_t)n);
}

void ws_bridge_install() {
    ws_server_set_rx_callback(ws_rx);
    event_bus_subscribe(EventType::ZCL_ATTR,    on_zcl_attr);
    // Command-driven optimistic changes ride the same forwarder → WS + relay so
    // no-report devices reflect in the SPA + cloud. The rule engine ignores it
    // (subscribes to ZCL_ATTR only). Mirrors main-core hap_dispatch.
    event_bus_subscribe(EventType::SHADOW_OPTIMISTIC, on_zcl_attr);
    event_bus_subscribe(EventType::DEVICE_JOIN, on_device_join);
    event_bus_subscribe(EventType::DEVICE_LEAVE, on_device_leave);
    ESP_LOGI(TAG, "WS rx (ping / status / device.{list,get,rename,delete,"
                   "attr.set,bind,reinterview,configure}) + "
                   "push (zcl_attr / device_join / device_leave) wired");
}
