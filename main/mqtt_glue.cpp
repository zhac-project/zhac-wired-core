// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_glue.cpp — see mqtt_glue.h. The dual-chip S3 does the same work in
// net-core's main.cpp + api_system.cpp; here the device data is local, so the
// Home Assistant callbacks read the pool and shadow directly.
#include "mqtt_glue.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "eth.h"
#include "event_bus.h"
#include "device_shadow.h"
#include "ha_bridge.h"
#include "mqtt_gw.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ws_bridge.h"
#include "zap_common.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"

static const char* TAG = "mqtt_glue";

namespace {

constexpr const char* kNs = "mqtt_cfg";   // same namespace + keys as net-core

struct Cfg {
    uint8_t enabled = 0;
    char    url[128] = {};
    char    root[32] = {};
    char    cid[32]  = {};
};

Cfg read_cfg() {
    Cfg c;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return c;
    nvs_get_u8(h, "enabled", &c.enabled);
    size_t n = sizeof(c.url);  nvs_get_str(h, "broker_url", c.url, &n);
    n = sizeof(c.root);        nvs_get_str(h, "root_topic", c.root, &n);
    n = sizeof(c.cid);         nvs_get_str(h, "client_id", c.cid, &n);
    nvs_close(h);
    return c;
}

void write_str(const char* key, const char* v) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void write_u8(const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

// mqtt://user:pass@host:1883 -> mqtt://host:1883, for status and logs.
void strip_userinfo(const char* in, char* out, size_t cap) {
    out[0] = '\0';
    if (!in || !in[0]) return;
    const char* sep  = strstr(in, "://");
    const char* host = sep ? sep + 3 : in;
    const char* at   = nullptr;
    for (const char* p = host; *p && *p != '/'; ++p) if (*p == '@') at = p;
    snprintf(out, cap, "%.*s%s", (int)(host - in), in, at ? at + 1 : host);
}

// ── Home Assistant data source ────────────────────────────────────────────

bool value_json(uint8_t type, int32_t int_val, const char* str_val, char* out, size_t cap) {
    switch (type) {
        case VAL_BOOL:  return snprintf(out, cap, "%d", int_val ? 1 : 0) > 0;
        case VAL_INT:   return snprintf(out, cap, "%ld", (long)int_val) > 0;
        case VAL_FLOAT: return ha::x100_to_json(out, cap, int_val) > 0;   // stored x100
        case VAL_STR: {
            JsonDocument d;
            d.set(str_val);
            const size_t n = serializeJson(d, out, cap);
            return n > 0 && n < cap;
        }
        default: return false;
    }
}

// Build one device's snapshot outside the pool lock (publishing can block on
// the broker), then hand it to ha_bridge.
bool with_device(uint64_t ieee, HaDeviceCb cb, void* ctx) {
    ZapDevice dev{};
    if (!zigbee_pool_snapshot(ieee, &dev) || zap_dev_is_removed(&dev)) return false;

    char vendor[32] = {}, model[32] = {};
    zhac_adapter_resolve_labels(dev.model_id, dev.manufacturer_name,
                                vendor, sizeof(vendor), model, sizeof(model));
    constexpr size_t kExp = 2048, kAttrs = 1536;
    char* exposes = static_cast<char*>(heap_caps_malloc(kExp, MALLOC_CAP_SPIRAM));
    char* attrs   = static_cast<char*>(heap_caps_malloc(kAttrs, MALLOC_CAP_SPIRAM));
    if (!exposes || !attrs) { heap_caps_free(exposes); heap_caps_free(attrs); return false; }
    if (zhac_adapter_build_exposes_json(ieee, dev.model_id, dev.manufacturer_name,
                                        exposes, kExp) == 0) {
        snprintf(exposes, kExp, "[]");
    }
    ShadowAttr sa[32];
    const uint8_t nsa = device_shadow_get_attrs(ieee, sa, 32);
    JsonDocument a;
    JsonObject obj = a.to<JsonObject>();
    for (uint8_t j = 0; j < nsa; j++) {
        char v[64];
        char s[ATTR_STR_MAX + 1] = {};                 // str_val need not end in NUL
        if (sa[j].val_type == VAL_STR) memcpy(s, sa[j].str_val, ATTR_STR_MAX);
        if (sa[j].key[0] == '_' || !value_json(sa[j].val_type, sa[j].int_val, s, v, sizeof(v)))
            continue;
        obj[sa[j].key] = serialized(v);
    }
    if (serializeJson(a, attrs, kAttrs) >= kAttrs) snprintf(attrs, kAttrs, "{}");

    const HaDeviceSnapshot snap{ieee, dev.friendly_name, vendor[0] ? vendor : dev.manufacturer_name,
                                model[0] ? model : dev.model_id, exposes, attrs};
    cb(snap, ctx);
    heap_caps_free(exposes);
    heap_caps_free(attrs);
    return true;
}

void for_each_device(HaDeviceCb cb, void* ctx) {
    // Copy the addresses first; with_device() snapshots each one again.
    uint64_t* ieees = static_cast<uint64_t*>(heap_caps_malloc(sizeof(uint64_t) * ZAP_MAX_DEVICES,
                                                              MALLOC_CAP_SPIRAM));
    if (!ieees) return;
    uint16_t n = 0;
    zigbee_pool_lock();
    ZapDevice* pool = pool_all();
    const uint16_t cnt = pool_count();
    for (uint16_t i = 0; pool && i < cnt && n < ZAP_MAX_DEVICES; i++) {
        if (!zap_dev_is_removed(&pool[i])) ieees[n++] = pool[i].ieee_addr;
    }
    zigbee_pool_unlock();
    for (uint16_t i = 0; i < n; i++) with_device(ieees[i], cb, ctx);
    heap_caps_free(ieees);
}

bool set_attr(uint64_t ieee, const char* key, const char* value_json_text) {
    JsonDocument v;
    if (deserializeJson(v, value_json_text)) return false;
    const char* err = nullptr;
    const bool ok = ws_bridge_attr_set(ieee, key, v.as<JsonVariantConst>(), &err);
    if (!ok) ESP_LOGW(TAG, "HA command %s=%s: %s", key, value_json_text, err ? err : "failed");
    return ok;
}

const HaBridgePlatform kPlatform = {
    "ZHAC wired (" CONFIG_IDF_TARGET ")",
    for_each_device,
    with_device,
    set_attr,
};

void on_attr(const Event& e) {
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    char v[64];
    char s[ATTR_STR_MAX + 1] = {};
    if (z.val_type == VAL_STR) memcpy(s, z.str_val, ATTR_STR_MAX);
    if (value_json(z.val_type, z.int_val, s, v, sizeof(v))) ha_bridge_publish_state(z.ieee, z.key, v);
}

// Everything under <root>/# that is not a Home Assistant command goes to the
// rule engine (Mqtt# triggers) and Lua (zhac.on_mqtt), as the dual-chip S3
// forwards MQTT_MSG_IN to the P4. Our own state echoes are skipped.
void on_mqtt_rx(const char* topic, int topic_len, const char* data, int data_len) {
    if (ha_bridge_on_mqtt_rx(topic, topic_len, data, data_len)) return;
    char own[48];
    const int ol = snprintf(own, sizeof(own), "%s/devices/", mqtt_gw_get_root_topic());
    if (ol > 0 && topic_len >= ol && strncmp(topic, own, (size_t)ol) == 0) return;
    Event ev{};
    ev.type = EventType::MQTT_MSG;
    auto& m = *reinterpret_cast<MqttMsgEvent*>(ev.data);
    const size_t tn = (size_t)topic_len < sizeof(m.topic) - 1 ? (size_t)topic_len : sizeof(m.topic) - 1;
    const size_t pn = (size_t)(data_len > 0 ? data_len : 0) < sizeof(m.payload) - 1
                          ? (size_t)(data_len > 0 ? data_len : 0) : sizeof(m.payload) - 1;
    memcpy(m.topic, topic, tn);
    m.topic[tn] = '\0';
    if (pn) memcpy(m.payload, data, pn);
    m.payload[pn] = '\0';
    event_bus_publish(ev);
}

void on_got_ip(void*, esp_event_base_t, int32_t, void*) {
    mqtt_gw_on_sta_up();   // starts the client ~5 s later if enabled + configured
}

}  // namespace

void mqtt_glue_start() {
    mqtt_gw_init();
    const Cfg c = read_cfg();
    if (c.root[0]) mqtt_gw_set_root_topic(c.root);
    if (c.enabled && c.url[0]) mqtt_gw_configure(c.url, c.root, c.cid);
    else if (c.cid[0])         mqtt_gw_set_client_id(c.cid);
    char safe[128];
    strip_userinfo(c.url, safe, sizeof(safe));
    ESP_LOGI(TAG, "MQTT %s broker=%s root=%s", (c.enabled && c.url[0]) ? "enabled" : "disabled",
             safe[0] ? safe : "(none)", mqtt_gw_get_root_topic());

    mqtt_gw_set_rx_callback(on_mqtt_rx);
    char sub[48];
    snprintf(sub, sizeof(sub), "%s/#", mqtt_gw_get_root_topic());
    mqtt_gw_subscribe(sub, 0);

    ha_bridge_init(&kPlatform);
    event_bus_subscribe(EventType::ZCL_ATTR, on_attr);
    event_bus_subscribe(EventType::SHADOW_OPTIMISTIC, on_attr);   // commands to no-report devices
    event_bus_subscribe(EventType::DEVICE_JOIN, [](const Event& e) {
        uint64_t ieee = 0;
        memcpy(&ieee, e.data, sizeof(ieee));
        ha_bridge_device_changed(ieee);
    });
    event_bus_subscribe(EventType::DEVICE_LEAVE, [](const Event& e) {
        uint64_t ieee = 0;
        memcpy(&ieee, e.data, sizeof(ieee));
        ha_bridge_device_removed(ieee);
    });

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr);
    NetStatus st{};
    eth_get_status(&st);
    if (st.has_ip) mqtt_gw_on_sta_up();   // the link beat us to it
}

void mqtt_glue_apply_settings(JsonDocument& doc) {
    const char* url  = doc["broker_url"]      | (const char*)nullptr;
    const char* root = doc["mqtt_root_topic"] | (const char*)nullptr;
    const char* cid  = doc["mqtt_client_id"]  | (const char*)nullptr;
    if (root && root[0] && strlen(root) < 32) { write_str("root_topic", root); mqtt_gw_set_root_topic(root); }
    if (cid && cid[0] && strlen(cid) < 32)    { write_str("client_id", cid);   mqtt_gw_set_client_id(cid); }
    if (url && url[0] && strlen(url) < 127) {
        write_str("broker_url", url);
        // Only a running client picks it up now; a disabled one gets it from
        // NVS when enabled (mqtt_gw_configure would arm it).
        if (read_cfg().enabled) mqtt_gw_set_broker_url(url);
    }
    if (doc["mqtt_enabled"].is<bool>()) {
        const bool en = doc["mqtt_enabled"].as<bool>();
        write_u8("enabled", en ? 1 : 0);
        if (en) {
            const Cfg c = read_cfg();
            if (c.url[0]) { mqtt_gw_configure(c.url, c.root, c.cid); mqtt_gw_on_sta_up(); }
        } else {
            mqtt_gw_stop();
        }
    }
    if (doc["ha_discovery"].is<bool>() || doc["ha_prefix"].is<const char*>()) {
        const bool en = doc["ha_discovery"] | ha_bridge_enabled();
        ha_bridge_configure(en, doc["ha_prefix"] | ha_bridge_prefix());
    }
}

void mqtt_glue_fill_status(JsonObject d) {
    const Cfg c = read_cfg();
    char safe[128];
    strip_userinfo(c.url, safe, sizeof(safe));
    d["mqtt_enabled"]   = c.enabled != 0;
    d["mqtt_broker"]    = safe;
    d["mqtt_client_id"] = c.cid;
    d["ha_discovery"]   = ha_bridge_enabled();
    d["ha_prefix"]      = ha_bridge_prefix();
}
