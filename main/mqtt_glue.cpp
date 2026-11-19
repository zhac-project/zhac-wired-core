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
#include "mqtt_gw_cfg.h"
#include "mqtt_payload.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ws_bridge.h"
#include "zap_common.h"
#include "zcl_attribute.h"   // zcl_attr_key_is_secret
#include "zhc_adapter.h"
#include "zigbee_pool.h"

static const char* TAG = "mqtt_glue";

namespace {

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

// The shadow's values as members of `obj`, without "_" bookkeeping keys and,
// with `state_only`, without what a retained state must not hold: secrets and
// the momentary action (ha::state_retained).
void add_attrs(JsonObject obj, const ShadowAttr* sa, uint8_t n, bool state_only) {
    for (uint8_t j = 0; j < n; j++) {
        char v[64];
        char s[ATTR_STR_MAX + 1] = {};                 // str_val need not end in NUL
        if (sa[j].val_type == VAL_STR) memcpy(s, sa[j].str_val, ATTR_STR_MAX);
        if (sa[j].key[0] == '_' || (state_only && !ha::state_retained(sa[j].key)) ||
            !value_json(sa[j].val_type, sa[j].int_val, s, v, sizeof(v)))
            continue;
        obj[sa[j].key] = serialized(v);
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
    add_attrs(a.to<JsonObject>(), sa, nsa, false);
    if (serializeJson(a, attrs, kAttrs) >= kAttrs) snprintf(attrs, kAttrs, "{}");

    const HaDeviceSnapshot snap{ieee, dev.friendly_name, vendor[0] ? vendor : dev.manufacturer_name,
                                model[0] ? model : dev.model_id, exposes, attrs,
                                ha_battery_powered(dev.power_source), zhac_adapter_device_offline(ieee)};
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
    if (!ok) ESP_LOGW(TAG, "HA command %s=%s: %s", key,
                      zcl_attr_key_is_secret(key) ? "<hidden>" : value_json_text,   // a lock PIN (ZR-06)
                      err ? err : "failed");
    return ok;
}

const HaBridgePlatform kPlatform = {
    "ZHAC wired (" CONFIG_IDF_TARGET ")",
    for_each_device,
    with_device,
    set_attr,
};

// z2m's per-device message: {"ieee":"0x..","attrs":{...}} with the device's
// whole retained-safe state, plus `key`=`val` (a press) when given. Length, or
// 0 when it does not fit `cap`.
size_t state_json(uint64_t ieee, const ShadowAttr* sa, uint8_t n, const char* key, const char* val,
                  char* out, size_t cap) {
    JsonDocument d;
    char id[19];
    snprintf(id, sizeof(id), "0x%016llX", (unsigned long long)ieee);
    d["ieee"] = id;
    JsonObject a = d["attrs"].to<JsonObject>();
    add_attrs(a, sa, n, true);
    if (key) a[key] = serialized(val);
    const size_t len = serializeJson(d, out, cap);
    return len < cap ? len : 0;
}

// <root>/devices/<IEEE>/state, read back from the shadow (research 2026-11-18
// A12). 4 KB: 32 attrs of at most ~95 B. PSRAM, so not on the bus pump's stack.
void publish_state(uint64_t ieee, const char* key, const char* val, bool retain) {
    char suffix[48], topic[96];
    snprintf(suffix, sizeof(suffix), "devices/%016llX/state", (unsigned long long)ieee);
    if (mqtt_gw_format_topic(topic, sizeof(topic), suffix) <= 0) return;
    constexpr size_t kCap = 4096;
    auto* sa  = static_cast<ShadowAttr*>(heap_caps_malloc(sizeof(ShadowAttr) * 32, MALLOC_CAP_SPIRAM));
    auto* out = static_cast<char*>(heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM));
    if (sa && out) {
        const uint8_t n   = device_shadow_get_attrs(ieee, sa, 32);
        const size_t  len = state_json(ieee, sa, n, key, val, out, kCap);
        if (len) mqtt_gw_publish(topic, out, len, 0, retain);
        else ESP_LOGW(TAG, "0x%016llX: state does not fit %u B", (unsigned long long)ieee, (unsigned)kCap);
    }
    heap_caps_free(sa);
    heap_caps_free(out);
}

// The plain per-device state topic, discovery or not, as z2m publishes it:
// the whole state once per report (the event with more == 0), retained, so a
// subscriber starts from the current state; a press (or another value a
// retained state must not hold) at once, with the state but not retained, so
// the broker never replays it.
void on_attr(const Event& e) {
    const auto& z = *reinterpret_cast<const ZclAttrEvent*>(e.data);
    char v[64];
    char s[ATTR_STR_MAX + 1] = {};
    if (z.val_type == VAL_STR) memcpy(s, z.str_val, ATTR_STR_MAX);
    const bool have = value_json(z.val_type, z.int_val, s, v, sizeof(v));
    if (have) ha_bridge_publish_state(z.ieee, z.key, v);   // Home Assistant topics, only while discovery is on
    if (!mqtt_gw_is_connected()) return;
    if (have && z.key[0] != '_' && !ha::state_retained(z.key)) publish_state(z.ieee, z.key, v, false);
    if (!z.more) publish_state(z.ieee, nullptr, nullptr, true);
}

// A removed device: its retained state goes too (an empty retained message),
// as z2m does -- else every new subscriber still sees it.
void on_device_leave(const Event& e) {
    uint64_t ieee = 0;
    memcpy(&ieee, e.data, sizeof(ieee));
    ha_bridge_device_removed(ieee);
    char suffix[48], topic[96];
    snprintf(suffix, sizeof(suffix), "devices/%016llX/state", (unsigned long long)ieee);
    if (mqtt_gw_format_topic(topic, sizeof(topic), suffix) > 0) mqtt_gw_publish(topic, "", 0, 0, true);
}

// The availability ping's verdict, to Home Assistant (research 2026-11-18 B7): DEVICE_OFFLINE when a mains device let
// its ping go unanswered, DEVICE_SEEN when it is heard again.
void on_availability(const Event& e) {
    uint64_t ieee = 0;
    memcpy(&ieee, e.data, sizeof(ieee));
    ha_bridge_device_online(ieee, e.type == EventType::DEVICE_SEEN);
}

// What the hub itself publishes under the root, back from the broker: device
// state, logs, metrics, availability. Never news to the rule engine or Lua --
// and a script that logs on every message would feed itself (research
// 2026-11-18 A12). Rule and script publishes are the user's: they come back.
bool own_echo(const char* topic, size_t len, const char* root) {
    static constexpr const char* kOwn[] = {"/devices/", "/log/", "/bridge/", "/availability"};
    const size_t rl = strlen(root);
    if (len <= rl || strncmp(topic, root, rl) != 0) return false;
    for (const char* o : kOwn) {
        const size_t ol = strlen(o);
        if (len - rl >= ol && strncmp(topic + rl, o, ol) == 0) return o[ol - 1] == '/' || len - rl == ol;
    }
    return false;
}

// Everything under <root>/# that is neither a command (ha_bridge takes /set)
// nor the hub's own echo goes to the rule engine (Mqtt# triggers) and Lua
// (zhac.on_mqtt), as the dual-chip S3 forwards MQTT_MSG_IN to the P4. A
// payload past the event's 31 bytes travels as its start plus the id of its
// whole text (mqtt_payload.h), which Lua reads.
void on_mqtt_rx(const char* topic, int topic_len, const char* data, int data_len) {
    if (ha_bridge_on_mqtt_rx(topic, topic_len, data, data_len)) return;
    if (own_echo(topic, (size_t)topic_len, mqtt_gw_get_root_topic())) return;
    Event ev{};
    ev.type = EventType::MQTT_MSG;
    auto& m = *reinterpret_cast<MqttMsgEvent*>(ev.data);
    const size_t tn = (size_t)topic_len < sizeof(m.topic) - 1 ? (size_t)topic_len : sizeof(m.topic) - 1;
    memcpy(m.topic, topic, tn);
    m.topic[tn] = '\0';
    mqtt_msg_set_payload(m, data, data_len > 0 ? (size_t)data_len : 0);
    event_bus_publish(ev);
}

void on_got_ip(void*, esp_event_base_t, int32_t, void*) {
    mqtt_gw_on_sta_up();   // starts the client ~5 s later if enabled + configured
}

// "<root>/#": commands, Home Assistant and MQTT-triggered rules. mqtt_gw keeps
// one filter, drops the previous one and re-subscribes it on every reconnect.
void subscribe_root() {
    char sub[80];
    snprintf(sub, sizeof(sub), "%s/#", mqtt_gw_get_root_topic());
    mqtt_gw_subscribe(sub, 0);
}

}  // namespace

void mqtt_glue_start() {
    mqtt_gw_init();
    mqtt_gw_cfg_boot();   // shared with mono: NVS settings, root topic, client id, arm if enabled

    mqtt_gw_set_rx_callback(on_mqtt_rx);
    subscribe_root();

    ha_bridge_init(&kPlatform);
    event_bus_subscribe(EventType::ZCL_ATTR, on_attr);
    event_bus_subscribe(EventType::SHADOW_OPTIMISTIC, on_attr);   // commands to no-report devices
    event_bus_subscribe(EventType::DEVICE_JOIN, [](const Event& e) {
        uint64_t ieee = 0;
        memcpy(&ieee, e.data, sizeof(ieee));
        ha_bridge_device_changed(ieee);
    });
    event_bus_subscribe(EventType::DEVICE_LEAVE, on_device_leave);
    event_bus_subscribe(EventType::DEVICE_OFFLINE, on_availability, nullptr, 4);   // at most 4 per ping tick
    event_bus_subscribe(EventType::DEVICE_SEEN, on_availability, nullptr, 4);      // one per answered ping

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr);
#ifdef CONFIG_ZHAC_WIFI_HOSTED
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr);
#endif
    NetStatus st{};
    eth_get_status(&st);
    if (st.has_ip) mqtt_gw_on_sta_up();   // the link beat us to it
}

void mqtt_glue_apply_settings(JsonDocument& doc) {
    char before[64];
    snprintf(before, sizeof(before), "%s", mqtt_gw_get_root_topic());
    mqtt_gw_cfg_apply(doc);
    // A new root must be listened to as well as published under: the old
    // "<root>/#" stayed until reboot, so commands and MQTT rules on the new
    // root never arrived (review F24).
    if (strcmp(before, mqtt_gw_get_root_topic()) != 0) subscribe_root();
    if (doc["ha_discovery"].is<bool>() || doc["ha_prefix"].is<const char*>()) {
        const bool en = doc["ha_discovery"] | ha_bridge_enabled();
        ha_bridge_configure(en, doc["ha_prefix"] | ha_bridge_prefix());
    }
}

void mqtt_glue_fill_status(JsonObject d) {
    mqtt_gw_cfg_fill_status(d);
    d["ha_discovery"] = ha_bridge_enabled();
    d["ha_prefix"]    = ha_bridge_prefix();
}
