// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// An interview works on a snapshot for seconds to minutes. What happened to
// the live record meanwhile must survive its commit (review F07).
#include "../../esp_zb_interview_merge.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static ZapDevice before() {   // the record when the interview took its snapshot
    ZapDevice d{};
    d.ieee_addr = 0xA4C1380000000001ull;
    d.nwk_addr  = 0x1111;
    d.last_seen = 1000;
    std::strcpy(d.friendly_name, "hall");
    return d;
}

int main() {
    // The interview's result: topology and identity read from the device.
    ZapDevice iv = before();
    iv.device_type = 2;
    iv.manufacturer_code = 0x1141;
    iv.endpoint_count = 1;
    iv.endpoints[0] = 1;
    iv.clusters[0][0] = 0x0500;
    std::strcpy(iv.model_id, "TS0202");
    std::strcpy(iv.manufacturer_name, "_TZ3000_abc");
    iv.power_source = 3;
    iv.interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_READY);
    iv.support_state = static_cast<uint8_t>(SupportState::MATCHED);
    iv.last_seen = 1005;

    {   // meanwhile: rejoin under a new address, configure done, renamed, seen later
        ZapDevice live = before();
        live.nwk_addr = 0x2222;
        live.configure_state = static_cast<uint8_t>(ConfigureState::DONE);
        std::strcpy(live.friendly_name, "hallway");
        live.last_seen = 1010;
        live.link_quality = 200;
        interview_merge(&live, iv);
        expect("a rejoin's newer address stays", live.nwk_addr == 0x2222);
        expect("configure state stays", live.configure_state == static_cast<uint8_t>(ConfigureState::DONE));
        expect("the user's name stays", std::strcmp(live.friendly_name, "hallway") == 0);
        expect("the later last_seen stays", live.last_seen == 1010);
        expect("link quality stays", live.link_quality == 200);
        expect("topology comes from the interview", live.endpoint_count == 1 && live.clusters[0][0] == 0x0500 &&
                                                    live.device_type == 2 && live.manufacturer_code == 0x1141);
        expect("identity comes from the interview", std::strcmp(live.model_id, "TS0202") == 0 &&
                                                    live.support_state == static_cast<uint8_t>(SupportState::MATCHED));
    }
    {   // meanwhile: a leave marked it removed
        ZapDevice live = before();
        zap_dev_mark_removed(&live);
        interview_merge(&live, iv);
        expect("a removal stays", zap_dev_is_removed(&live));
    }
    {   // meanwhile: a late Basic report brought the identity this attempt missed
        ZapDevice no_id = iv;
        no_id.model_id[0] = no_id.manufacturer_name[0] = '\0';
        no_id.interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_PENDING);
        no_id.support_state = static_cast<uint8_t>(SupportState::UNKNOWN);
        ZapDevice live = before();
        std::strcpy(live.model_id, "TS0202");
        live.interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_READY);
        live.support_state = static_cast<uint8_t>(SupportState::MATCHED);
        interview_merge(&live, no_id);
        expect("the late identity stays", std::strcmp(live.model_id, "TS0202") == 0);
        expect("  and its states", live.interview_state == static_cast<uint8_t>(InterviewState::IDENTITY_READY) &&
                                   live.support_state == static_cast<uint8_t>(SupportState::MATCHED));
    }
    {   // a record that never had an address takes the interview's
        ZapDevice live = before();
        live.nwk_addr = 0;
        interview_merge(&live, iv);
        expect("an empty address is filled", live.nwk_addr == 0x1111);
    }

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\ninterview merge OK\n", g_fail);
    return g_fail;
}
