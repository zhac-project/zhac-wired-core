// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_interview_merge -- what an interview owns of a device record.
//
// An interview works on a snapshot for seconds to minutes. Meanwhile a rejoin
// moves the short address, a late Basic report brings the identity, a leave
// marks the device removed, configure finishes, reports refresh last_seen and
// link quality. Copying the whole snapshot back threw all of that away and
// persisted the stale record (review F07). Only the fields the interview read
// are merged; everything else stays as the live record has it. Call under the
// pool lock.
#pragma once

#include <cstring>

#include "zap_common.h"

inline void interview_merge(ZapDevice* live, const ZapDevice& iv) {
    // Topology: this interview just read it.
    live->device_type       = iv.device_type;
    live->manufacturer_code = iv.manufacturer_code;
    live->endpoint_count    = iv.endpoint_count;
    std::memcpy(live->endpoints,    iv.endpoints,    sizeof(live->endpoints));
    std::memcpy(live->clusters,     iv.clusters,     sizeof(live->clusters));
    std::memcpy(live->clusters_out, iv.clusters_out, sizeof(live->clusters_out));

    // Identity and what follows from it -- unless this attempt found none and
    // the live record got one meanwhile (a Basic report on the late path).
    const bool iv_identity   = iv.model_id[0] || iv.manufacturer_name[0];
    const bool live_identity = live->model_id[0] || live->manufacturer_name[0];
    if (iv_identity || !live_identity) {
        std::memcpy(live->model_id, iv.model_id, sizeof(live->model_id));
        std::memcpy(live->manufacturer_name, iv.manufacturer_name, sizeof(live->manufacturer_name));
        live->interview_state = iv.interview_state;
        live->support_state   = iv.support_state;
        if (iv.power_source) live->power_source = iv.power_source;
    }

    if (!live->nwk_addr) live->nwk_addr = iv.nwk_addr;     // a rejoin's newer address wins
    if (iv.last_seen > live->last_seen) live->last_seen = iv.last_seen;
}
