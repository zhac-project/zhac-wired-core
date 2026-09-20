// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_glue — this SKU's MQTT client wiring: settings in NVS `mqtt_cfg`
// (same keys as the dual-chip S3), start on Ethernet link-up, inbound
// messages to rules / Lua / Home Assistant, and ha_bridge's data source.
#pragma once
#include "ArduinoJson.h"

// After event_bus, the device pipeline and the radio are up.
void mqtt_glue_start();

// settings.set keys: broker_url, mqtt_root_topic, mqtt_client_id,
// mqtt_enabled, ha_discovery, ha_prefix. Persists and applies each present.
void mqtt_glue_apply_settings(JsonDocument& doc);

// status keys: mqtt_enabled, mqtt_broker (credentials removed),
// mqtt_client_id, ha_discovery, ha_prefix.
void mqtt_glue_fill_status(JsonObject d);
