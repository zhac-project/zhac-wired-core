// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_backend -- esp-zigbee-lib (v2.x) as ZHAC's radio layer.
//
// THE LOAD-BEARING IDEA
// ---------------------
// ZHAC's product is "dumb stack on the radio, device intelligence on the host":
// 6,235 device definitions decode raw ZCL in embedded-zhc. That only works if
// something hands us the raw APS payload. esp-zigbee-lib does, via
// ezb_apsde_data_indication_handler_register() -- the callback receives the
// ASDU bytes plus src address, endpoints, cluster, profile and LQI, and its
// return value decides whether the stack also processes the frame.
//
// That callback maps onto zhac_adapter_try_decode() parameter-for-parameter,
// which is why this backend is thin. Returning false lets the stack continue
// handling the frame normally (ZDO, ZCL foundation) even when we decoded it --
// deliberately conservative: ZHAC observes, it does not swallow.
//
// BOTH SKUs, ONE FILE
// -------------------
// The only difference is where the PHY is, and that is a config field:
//   esp32s31 -> ESP_ZIGBEE_RADIO_MODE_NATIVE    (own 802.15.4)
//   esp32p4  -> ESP_ZIGBEE_RADIO_MODE_UART_RCP  (C6 running stock ot_rcp)
// Everything above is identical, which was the entire argument for keeping
// both SKUs in one repo.
#include "esp_zigbee_backend.h"
#include "esp_zb_interview.h"
#include "sdkconfig.h"

#if CONFIG_ZHAC_ESP_ZIGBEE

#include "device_backend.h"
#include "esp_log.h"
#include "esp_system.h"   // esp_restart
#include "esp_timer.h"
#include "event_bus.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/platform/radio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "task_stacks.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "zigbee_diagnostics.h"
#include "zigbee_configure_queue.h"
#include "zigbee_identity.h"
#include "zigbee_pool.h"

// Defined in zhac-components' zhc_shadow_bridge.cpp; declared in no header.
extern "C" void zhc_shadow_bridge_register(void);

#include <cstring>

static const char* TAG = "esp_zb";

static bool s_running = false;
// Set from on_app_signal: true once a PAN exists (formed now, or resumed from
// the dataset). Opening the network before this fails inside the stack.
static bool s_formed  = false;

// Defined below, used by zb_init() above its definition.
static bool zb_network_ready();

// A mainloop exit below this uptime is treated as a startup failure and does
// NOT trigger a reboot -- see task_zigbee().
static constexpr int kMainloopRebootMinUptimeS = 60;

// ── Raw APS ingress: the make-or-break path (gate G2) ────────────────────
//
// Every inbound application frame lands here with its ASDU intact. We look the
// device up in the pool for the identity strings the definition matcher needs
// (modelId + manufacturerName), tell the adapter the (ieee, nwk) tuple so
// converters that reply have a destination, then decode.
// zigbee_identity_on_af_incoming() takes a ZNP AF_INCOMING_MSG payload, not a
// raw ASDU -- it reads cluster at [2], src nwk at [4], data_len at [16], body
// from [17]. Everything BEHIND that entry point (the worker task, pool update,
// re-match, persist) is transport-neutral and well tested, so rather than
// duplicate that logic we hand it a synthetic header.
//
// Only the four fields it actually reads are filled; the rest stay zero. If
// that layout ever changes upstream this breaks silently, which is why the
// better fix is a transport-neutral entry point in zhac-components -- raised,
// not done here, because this SKU cannot modify that repo.
static void feed_late_identity(uint16_t nwk, uint16_t cluster_id,
                               const uint8_t* zcl, uint16_t zcl_len) {
    if (cluster_id != 0x0000 || !zcl || zcl_len == 0) return;
    if (zcl_len > 200) return;                 // keep the frame off the stack

    uint8_t af[17 + 200] = {};
    af[2]  = static_cast<uint8_t>(cluster_id & 0xFF);
    af[3]  = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af[4]  = static_cast<uint8_t>(nwk & 0xFF);
    af[5]  = static_cast<uint8_t>((nwk >> 8) & 0xFF);
    af[16] = static_cast<uint8_t>(zcl_len);
    std::memcpy(af + 17, zcl, zcl_len);
    zigbee_identity_on_af_incoming(af, static_cast<uint8_t>(17 + zcl_len));
}

static bool on_apsde_indication(const ezb_apsde_data_ind_t* ind) {
    if (!ind || !ind->asdu || ind->asdu_length == 0) return false;

    // Source may arrive short or extended; the pool is keyed by IEEE.
    uint64_t ieee = 0;
    uint16_t nwk  = 0;
    if (ind->src_address.addr_mode == EZB_ADDR_MODE_EXT) {
        // ezb_extaddr_t is a packed union struct, not a raw byte array.
        ieee = ind->src_address.u.extended_addr.u64;
        ZapDevice snap{};
        if (zigbee_pool_snapshot(ieee, &snap)) nwk = snap.nwk_addr;
    } else {
        nwk = ind->src_address.u.short_addr;
        ZapDevice snap{};
        if (zigbee_pool_snapshot_by_nwk(nwk, &snap)) ieee = snap.ieee_addr;
    }

    if (ieee == 0) {
        // Frame from a device not in the pool: nothing to match a definition
        // against yet. Record it so /api/diagnostics/unhandled shows it rather
        // than it vanishing. ZCL byte 0 is frame control (bit0 = cluster
        // specific); the attr/cmd id sits after the 1-byte TSN.
        const bool cluster_specific = (ind->asdu[0] & 0x01) != 0;
        const uint16_t attr_or_cmd = (ind->asdu_length >= 3) ? ind->asdu[2] : 0;
        zb_diag_record_unhandled(ind->cluster_id, attr_or_cmd, cluster_specific, 0);
        return false;
    }

    // Link quality. Every APS indication carries it and nothing else on this
    // backend ever wrote it, so the UI showed lqi 0 for every device forever.
    // Cheap in-place update under the pool's own visitor lock.
    if (ieee != 0 && ind->lqi != 0) {
        struct LqiCtx { uint8_t lqi; uint32_t now; };
        LqiCtx lc{ind->lqi, static_cast<uint32_t>(esp_timer_get_time() / 1000000)};
        zigbee_pool_with_device(ieee, [](ZapDevice* d, void* c) {
            auto* x = static_cast<LqiCtx*>(c);
            d->link_quality = x->lqi;
            d->last_seen    = x->now;
        }, &lc);
    }

    // Give the interview engine first sight of the frame. It only consumes
    // Basic-cluster replies it is actively waiting for; everything continues
    // to the adapter either way.
    zb_interview_feed_zcl(nwk, ind->cluster_id, ind->src_endpoint,
                          ind->asdu, static_cast<uint8_t>(ind->asdu_length));
    zb_groups_feed_zcl(nwk, ind->cluster_id, ind->asdu,
                       static_cast<uint8_t>(ind->asdu_length));
    feed_late_identity(nwk, ind->cluster_id, ind->asdu, ind->asdu_length);

    // Identity strings for the matcher, straight from the pool snapshot.
    ZapDevice dev{};
    const bool have = zigbee_pool_snapshot(ieee, &dev);
    const char* model = (have && dev.model_id[0]) ? dev.model_id : nullptr;
    const char* manuf = (have && dev.manufacturer_name[0]) ? dev.manufacturer_name : nullptr;

    zhac_adapter_set_runtime_addr(ieee, nwk);

    const bool decoded = zhac_adapter_try_decode(
        ieee, model, manuf,
        /*group_id=*/0,
        ind->cluster_id, ind->src_endpoint, ind->lqi,
        ind->asdu, ind->asdu_length);

    if (!decoded) {
        const bool cluster_specific = (ind->asdu[0] & 0x01) != 0;
        const uint16_t attr_or_cmd = (ind->asdu_length >= 3) ? ind->asdu[2] : 0;
        zb_diag_record_unhandled(ind->cluster_id, attr_or_cmd, cluster_specific, ieee);
    }

    // false = "we did not consume it". ZHAC decodes for its own shadow; the
    // stack still owes the device its normal ZCL/ZDO handling.
    return false;
}

// ── Egress: adapter-encoded ZCL out over APS ─────────────────────────────
// Non-static: esp_zb_interview.cpp sends its Basic-cluster reads through this
// rather than duplicating the apsde plumbing (declared in esp_zb_interview.h).
bool esp_zb_af_send(uint16_t nwk_addr, uint8_t dst_ep, uint16_t cluster_id,
                    const uint8_t* zcl_data, size_t zcl_len) {
    if (!zcl_data || zcl_len == 0) return false;

    ezb_apsde_data_req_t req{};
    req.dst_address.addr_mode   = EZB_ADDR_MODE_SHORT;
    req.dst_address.u.short_addr = nwk_addr;
    req.src_endpoint = 1;
    req.dst_endpoint = dst_ep ? dst_ep : 1;
    req.cluster_id   = cluster_id;
    req.profile_id   = 0x0104;   // Home Automation
    req.radius       = 0;
    req.tx_options   = EZB_APSDE_TX_OPT_ACK_TX;
    req.asdu_length  = static_cast<uint16_t>(zcl_len);
    req.asdu         = const_cast<uint8_t*>(zcl_data);

    const ezb_err_t err = ezb_apsde_data_request(&req);
    if (err != 0) {
        ESP_LOGW(TAG, "apsde_data_request to 0x%04x cluster 0x%04x failed (%d)",
                 nwk_addr, cluster_id, (int)err);
        return false;
    }
    return true;
}

// ── Stack lifecycle signals ──────────────────────────────────────────────
//
// This handler is not optional decoration -- it DRIVES commissioning.
// esp_zigbee_start(true) runs BDB *initialisation* only: the stack reads the
// dataset, decides whether a network already exists, and then raises a signal
// asking the application what to do. If nobody answers, the coordinator sits
// there forever with no PAN, and ezb_bdb_open_network() fails with no log of
// its own -- which is exactly how this surfaced: REST returned
// "500 permit_join failed" while the boot log looked perfectly healthy.
static bool on_app_signal(const ezb_app_signal_t* app_signal) {
    if (!app_signal) return false;
    const ezb_app_signal_type_t sig = ezb_app_signal_get_type(app_signal);

    switch (sig) {
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START: {
        // Empty dataset -- form a new PAN. First boot after a flash erase.
        ESP_LOGI(TAG, "no network in dataset -- forming");
        const ezb_err_t e =
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        if (e != 0) {
            ESP_LOGE(TAG, "formation start failed (%d), bdb status %u",
                     (int)e, (unsigned)ezb_bdb_get_commissioning_status());
        }
        return true;
    }

    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        // Dataset already held a network and the stack came back up on it.
        // No formation needed; joined devices keep their addresses.
        s_formed = true;
        ESP_LOGI(TAG, "resumed network: pan 0x%04x channel %u",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel());
        return true;

    case EZB_BDB_SIGNAL_FORMATION:
        s_formed = true;
        ESP_LOGI(TAG, "network formed: pan 0x%04x channel %u",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel());
        return true;

    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        // A device joined (or a known one rejoined) and announced itself.
        // Queue it; the interview task owns everything after this point.
        const auto* p = static_cast<const ezb_zdo_signal_device_annce_params_t*>(
            ezb_app_signal_get_params(app_signal));
        if (p) {
            ESP_LOGI(TAG, "device announce: nwk 0x%04x ieee %016llx caps 0x%02x",
                     (unsigned)p->short_addr,
                     (unsigned long long)p->device_addr.u64,
                     (unsigned)p->capability);
            zb_interview_on_announce(p->device_addr.u64, p->short_addr);
        }
        return true;
    }

    case EZB_ZDO_SIGNAL_LEAVE:
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        // Both carry the same params struct. LEAVE is this device being told
        // to leave; LEAVE_INDICATION is a child/neighbour announcing it left.
        // For a coordinator only the latter is expected, but handling both
        // means a stack that reclassifies the event cannot silently strand a
        // device in the pool as permanently present.
        const auto* p = static_cast<const ezb_zdo_signal_leave_indication_params_t*>(
            ezb_app_signal_get_params(app_signal));
        if (p) {
            ESP_LOGI(TAG, "leave: nwk 0x%04x ieee %016llx type %u",
                     (unsigned)p->short_addr,
                     (unsigned long long)p->device_addr.u64,
                     (unsigned)p->leave_type);
            zb_interview_on_leave(p->device_addr.u64);
        }
        return true;
    }

    default:
        // INFO, not DEBUG: this is the only evidence that signals are being
        // delivered at all, and ESP_LOGD is compiled out at the default
        // CONFIG_LOG_MAXIMUM_LEVEL=INFO -- so a DEBUG line here is invisible
        // exactly when it matters.
        ESP_LOGI(TAG, "signal %s (0x%04x)", ezb_app_signal_to_string(sig),
                 (unsigned)sig);
        return false;
    }
}

// ── The Zigbee task ──────────────────────────────────────────────────────
// esp_zigbee_launch_mainloop() does not return; it needs its own task.
static void task_zigbee(void*) {
    const int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "stack mainloop starting");

    // Does not return in normal operation.
    const esp_err_t err = esp_zigbee_launch_mainloop();

    // Getting here means the radio is gone: no joins, no reports, no commands.
    // Nothing in the stack recovers from this by itself, and leaving the
    // firmware "up" with a dead radio is the worst outcome -- REST answers,
    // the SPA renders, and every device silently stops working.
    s_running = false;
    s_formed  = false;
    const int64_t uptime_s = (esp_timer_get_time() - started_us) / 1000000;
    ESP_LOGE(TAG, "stack mainloop EXITED after %llds: %s -- radio is down",
             (long long)uptime_s, esp_err_to_name(err));

    // Reboot to recover, but only if the stack had actually been running for a
    // while. A stack that dies within the first minute would otherwise turn
    // every boot into a reset loop, which is strictly worse than staying up
    // with is_running() == false: that at least leaves REST reachable to say
    // so, and leaves the console usable to diagnose it.
    if (uptime_s >= kMainloopRebootMinUptimeS) {
        ESP_LOGE(TAG, "restarting to recover the radio");
        vTaskDelay(pdMS_TO_TICKS(1000));   // let the log drain
        esp_restart();
    }
    ESP_LOGE(TAG, "died %llds into the run (< %ds) -- NOT restarting, to avoid "
                  "a boot loop. /api/status reports the radio as down.",
             (long long)uptime_s, kMainloopRebootMinUptimeS);
    vTaskDelete(nullptr);
}

// ── DeviceBackend implementation ─────────────────────────────────────────

static bool zb_init() {
    // The dataset partition must be an initialised NVS partition BEFORE
    // esp_zigbee_init: ezb_plat_datasets_init calls nvs_open_from_partition()
    // and abort()s on failure. Note esp-zigbee-lib v2.x uses NVS here, unlike
    // v1.x (and IDF's bundled examples) which used a FAT partition.
    esp_err_t nerr = nvs_flash_init_partition(CONFIG_ZHAC_ZB_STORAGE_PARTITION);
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(CONFIG_ZHAC_ZB_STORAGE_PARTITION));
        nerr = nvs_flash_init_partition(CONFIG_ZHAC_ZB_STORAGE_PARTITION);
    }
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "zigbee storage partition '%s' unavailable: %s -- radio down",
                 CONFIG_ZHAC_ZB_STORAGE_PARTITION, esp_err_to_name(nerr));
        return false;
    }

    // The device pool is normally brought up by zigbee_mgr_init(), which is
    // ZNP-bound and therefore not compiled into this SKU. Nothing else calls
    // these, so without them pool_add() would write through a null pool and
    // every join would be lost. Order matters: init (allocates in PSRAM) ->
    // snapshot cb (so deferred flushes can read live state) -> restore.
    zigbee_pool_init();
    zap_store_set_snapshot_cb(zigbee_pool_snapshot);

    // Install the decode -> device_shadow sink. Normally done by
    // zigbee_mgr_init(), which is ZNP-bound and not compiled here. Must happen
    // BEFORE the radio starts: zhac_adapter_try_decode() drops every key it
    // decodes if no sink is registered, so any frame arriving in the gap is
    // logged as "matched" and then silently lost.
    zhc_shadow_bridge_register();
    const uint16_t restored = zigbee_pool_restore_persisted();
    if (restored) {
        ESP_LOGI(TAG, "restored %u device(s) from NVS", (unsigned)restored);
    }

    esp_zigbee_config_t cfg{};
    cfg.device_config.device_type         = EZB_NWK_DEVICE_TYPE_COORDINATOR;
    cfg.device_config.install_code_policy = false;
    cfg.device_config.zczr_config.max_children = CONFIG_ZHAC_ZB_MAX_CHILDREN;
    cfg.platform_config.storage_partition_name = CONFIG_ZHAC_ZB_STORAGE_PARTITION;

#if CONFIG_ZB_RADIO_NATIVE
    cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;
    ESP_LOGI(TAG, "radio: native 802.15.4");
#else
    cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP;
    auto& u = cfg.platform_config.radio_config.radio_uart_config;
    u.port   = static_cast<uart_port_t>(CONFIG_ZHAC_RCP_UART_PORT);
    u.rx_pin = static_cast<gpio_num_t>(CONFIG_ZHAC_RCP_UART_RX_GPIO);
    u.tx_pin = static_cast<gpio_num_t>(CONFIG_ZHAC_RCP_UART_TX_GPIO);
    u.uart_config.baud_rate = 460800;
    u.uart_config.data_bits = UART_DATA_8_BITS;
    u.uart_config.parity    = UART_PARITY_DISABLE;
    u.uart_config.stop_bits = UART_STOP_BITS_1;
    u.uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    u.uart_config.rx_flow_ctrl_thresh = 0;
    u.uart_config.source_clk = UART_SCLK_DEFAULT;
    ESP_LOGI(TAG, "radio: RCP over UART%d (tx=%d rx=%d @460800)",
             CONFIG_ZHAC_RCP_UART_PORT, CONFIG_ZHAC_RCP_UART_TX_GPIO,
             CONFIG_ZHAC_RCP_UART_RX_GPIO);
#endif

    esp_err_t err = esp_zigbee_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Hooks BEFORE start, so nothing is missed between start and registration.
    ezb_apsde_data_indication_handler_register(on_apsde_indication);
    const ezb_err_t sig_err = ezb_app_signal_add_handler(on_app_signal);
    if (sig_err != 0) {
        ESP_LOGE(TAG, "app signal handler registration FAILED (%d) -- "
                      "commissioning cannot be driven", (int)sig_err);
    }
    zhac_adapter_register_send(esp_zb_af_send);

    ezb_bdb_set_primary_channel_set(CONFIG_ZHAC_ZB_CHANNEL_MASK);

    // autostart=true: run the normal BDB startup. v2.x does NOT auto-run
    // initialisation otherwise, so a reboot would silently not rejoin.
    err = esp_zigbee_start(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s", esp_err_to_name(err));
        return false;
    }

    if (xTaskCreate(task_zigbee, "TaskZigbee", zhac::stack::kEventBus,
                    nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "TaskZigbee create failed -- stack will not run");
        return false;
    }

    s_running = true;

    // Wait for autostart to produce a PAN, then form explicitly if it did not.
    //
    // esp_zigbee_start(true) is documented to run the whole startup procedure
    // "including formation", and on a fresh coordinator it should. It does not
    // here: the PAN id stays 0xffff and bdb commissioning status stays 0
    // indefinitely (observed over 38 s). No signal is emitted either -- not
    // FORMATION, not DEVICE_FIRST_START, not anything -- because signals are a
    // product of commissioning, and commissioning never began. So there is
    // nothing to wait for and nothing to notice the failure.
    //
    // Kicking NETWORK_FORMATION explicitly fixes it: the PAN appears in ~4 s
    // and the FORMATION signal then arrives normally (pan 0x27bf channel 12 on
    // the bench). The poll-then-kick shape keeps the fast path when a dataset
    // already holds a network, where autostart DOES rejoin on its own.
    for (int i = 0; i < 30 && !zb_network_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!zb_network_ready()) {
        ESP_LOGW(TAG, "no PAN after autostart -- forming explicitly");
        const ezb_err_t fe =
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        if (fe != 0) {
            ESP_LOGE(TAG, "formation start failed (%d)", (int)fe);
        } else {
            for (int i = 0; i < 100 && !zb_network_ready(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    } else {
        // Resume path: the dataset already held a PAN and the stack came back
        // up on it. Tempting to do nothing here -- the network exists and
        // frames flow -- but BDB itself never ran, and it refuses to act from
        // that state. ezb_bdb_open_network() returns 3 (EZB_ERR_INV_STATE), so
        // permit_join fails on every boot after the first with a network that
        // looks perfectly healthy.
        //
        // BDB INITIALIZATION is the spec's answer for "device restarted with
        // network parameters already in NVRAM"; it also raises DEVICE_REBOOT.
        const ezb_err_t ie =
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        if (ie != 0) {
            ESP_LOGW(TAG, "bdb initialization failed (%d) -- permit_join may "
                          "refuse until the next reboot", (int)ie);
        }
    }

    // Only start join handling once there is a PAN to join. Configure
    // transports must be registered before any interview can finish, since
    // the interview calls zhac_adapter_configure() on success.
    esp_zb_configure_register();
    zb_interview_init();
    // Late-identity enrichment + deferred configure retries. Both are started
    // by zigbee_mgr_init() on the ZNP path; neither is optional for battery
    // devices, which routinely sleep through the interview's Basic read and
    // through the first configure attempt.
    zigbee_identity_init();
    zigbee_configure_init();

    if (zb_network_ready()) {
        s_formed = true;
        ESP_LOGI(TAG, "coordinator up: pan 0x%04x channel %u max_children=%d",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel(),
                 CONFIG_ZHAC_ZB_MAX_CHILDREN);
    } else {
        // Honest: the backend is registered and the stack runs, but joins are
        // impossible until a PAN exists. permit_join will say so too.
        ESP_LOGE(TAG, "coordinator has NO network (pan 0x%04x bdb %u) -- "
                      "joins will be refused",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_bdb_get_commissioning_status());
    }
    return true;
}

static bool zb_is_running() { return s_running; }

// Is there actually a PAN?
//
// s_formed is set from on_app_signal, but that signal only ever fires once
// formation has been STARTED. On this lib build esp_zigbee_start(true) never
// starts it (see zb_init), so for the first ~5 s no signal exists to wait for
// and s_formed alone would leave permit_join refused forever. Asking the stack
// directly is the authoritative check; the signal is the fast path that keeps
// it cheap afterwards.
//
// 0x0000 and 0xffff are both "no network": 0xffff is the broadcast PAN id the
// stack holds before commissioning.
static bool zb_network_ready() {
    if (s_formed) return true;
    const ezb_panid_t pan = ezb_nwk_get_panid();
    return pan != 0x0000 && pan != 0xffff;
}

static bool zb_start_discovery(uint8_t duration_s) {
    if (!s_running) {
        ESP_LOGW(TAG, "permit_join refused: stack not running");
        return false;
    }
    if (!zb_network_ready()) {
        // Distinguishes "still forming" from a genuine stack error. Without
        // this the REST layer's flat 500 was the only symptom.
        ESP_LOGW(TAG, "permit_join refused: no network (pan 0x%04x ch %u bdb %u)",
                 (unsigned)ezb_nwk_get_panid(),
                 (unsigned)ezb_nwk_get_current_channel(),
                 (unsigned)ezb_bdb_get_commissioning_status());
        return false;
    }
    const ezb_err_t e = ezb_bdb_open_network(duration_s);
    if (e != 0) {
        ESP_LOGW(TAG, "open_network(%us) failed (%d)", (unsigned)duration_s, (int)e);
        return false;
    }
    ESP_LOGI(TAG, "network open for %us", (unsigned)duration_s);
    return true;
}

static bool zb_stop_discovery() {
    if (!s_running || !zb_network_ready()) return false;
    const ezb_err_t e = ezb_bdb_close_network();
    if (e != 0) {
        ESP_LOGW(TAG, "close_network failed (%d)", (int)e);
        return false;
    }
    ESP_LOGI(TAG, "network closed");
    return true;
}

// Tell the device to leave, then forget it locally.
//
// The local removal is unconditional and does not wait for the ZDO response:
// the overwhelmingly common reason to remove a device is that it is already
// dead or out of range, and a UI delete that fails because the device cannot
// be reached is useless. z2m behaves the same way.
static bool zb_backend_remove_device(uint64_t ieee) {
    if (s_running && zb_network_ready()) {
        ZapDevice snap{};
        if (zigbee_pool_snapshot(ieee, &snap) && snap.nwk_addr) {
            ezb_zdo_nwk_mgmt_leave_req_t req{};
            req.dst_nwk_addr = snap.nwk_addr;
            req.field.device_addr.u64 = ieee;
            req.field.remove_children = false;   // reassign, do not orphan-purge
            req.field.rejoin          = false;
            const ezb_err_t e = ezb_zdo_nwk_mgmt_leave_req(&req);
            if (e != 0) {
                ESP_LOGW(TAG, "leave req for %016llx failed (%d) -- removing "
                              "locally anyway", (unsigned long long)ieee, (int)e);
            }
        }
    }
    return zb_interview_forget(ieee);
}

static bool zb_backend_interview(uint64_t ieee, uint16_t addr_hint) {
    if (!s_running) return false;
    // A device already in the pool goes through trigger (which reads its
    // current short address). An unknown IEEE with a usable hint is admitted
    // as a fresh join -- that is what the REST "add by address" path wants.
    if (zb_interview_trigger(ieee)) return true;
    if (addr_hint == 0) return false;
    zb_interview_enqueue(ieee, addr_hint);
    return true;
}

static bool zb_get_device_list(ZapDevice* out, uint16_t max, uint16_t* count_out) {
    if (!out || !count_out) return false;
    *count_out = 0;
    zigbee_pool_lock();
    const ZapDevice* all = pool_all();
    const uint16_t n = pool_count();
    uint16_t w = 0;
    for (uint16_t i = 0; i < n && w < max; i++) {
        if (all[i].ieee_addr) out[w++] = all[i];
    }
    zigbee_pool_unlock();
    *count_out = w;
    return true;
}

static bool zb_get_device(uint64_t ieee, ZapDevice* out) {
    return out && zigbee_pool_snapshot(ieee, out);
}

static bool zb_write_attr(uint64_t ieee, uint8_t ep, const char* key, int32_t val) {
    // The adapter owns encoding and calls zb_af_send once it has a frame, but
    // it needs the device's identity to choose a converter and its nwk address
    // to address the frame -- so read the pool first.
    ZapDevice dev{};
    if (!zigbee_pool_snapshot(ieee, &dev)) {
        ESP_LOGW(TAG, "write_attr: %016llx not in pool", (unsigned long long)ieee);
        return false;
    }
    const uint8_t dst_ep = ep ? ep : dev.endpoints[0];
    return zhac_adapter_send_uint(ieee,
                                  dev.model_id[0] ? dev.model_id : nullptr,
                                  dev.manufacturer_name[0] ? dev.manufacturer_name : nullptr,
                                  dev.nwk_addr, dst_ep, key,
                                  static_cast<uint64_t>(val));
}

static DeviceBackend s_backend = {
    .protocol        = PROTO_ZIGBEE,
    .name            = "Zigbee",
    .init            = zb_init,
    // No poll: the stack owns TaskZigbee and its own transport threads.
    .poll            = nullptr,
    .is_running      = zb_is_running,
    .start_discovery = zb_start_discovery,
    .stop_discovery  = zb_stop_discovery,
    // remove / rename still need ZDO round-trips this backend does not issue
    // yet; nullptr stays honest for those.
    .interview       = zb_backend_interview,
    .write_attr      = zb_write_attr,
    .read_attr       = nullptr,
    .get_device_list = zb_get_device_list,
    .get_device      = zb_get_device,
    .remove_device   = zb_backend_remove_device,
    .rename_device   = nullptr,
};

bool esp_zigbee_backend_register(void) {
    if (!device_backend_register(&s_backend)) {
        ESP_LOGE(TAG, "device_backend_register failed");
        return false;
    }
    ESP_LOGI(TAG, "registered as DeviceBackend");
    return true;
}

// ── Radio entry points the control surface calls directly ────────────────
// These replace components/zigbee_mgr/radio_stubs.cpp, which is dropped from
// the build when CONFIG_ZHAC_ESP_ZIGBEE is set. Same signatures as the
// sibling zigbee_mgr declares in zigbee_mgr.h.
bool zigbee_permit_join(uint8_t duration_s) { return zb_start_discovery(duration_s); }

uint64_t zigbee_mgr_coordinator_ieee() {
    ezb_extaddr_t addr{};
    ezb_plat_radio_get_macaddr(addr.u8);
    return addr.u64;
}

// Not yet implemented on this backend -- ZDO bind/unbind and recommission need
// the ZDO request path, which lands with the join-handling increment. Refusing
// loudly beats pretending.
static inline bool not_yet(const char* op) {
    ESP_LOGW(TAG, "%s: not implemented on esp_zigbee_backend yet", op);
    return false;
}
bool zigbee_zdo_bind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                     uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    if (!s_running) return false;
    return esp_zb_zdo_bind(src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep,
                           /*unbind=*/false);
}
bool zigbee_zdo_unbind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                       uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    if (!s_running) return false;
    return esp_zb_zdo_bind(src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep,
                           /*unbind=*/true);
}
bool zigbee_interview_trigger(uint64_t ieee) { return zb_interview_trigger(ieee); }
bool zigbee_force_recommission() { return not_yet("force_recommission"); }

#else   // !CONFIG_ZHAC_ESP_ZIGBEE

// Radio disabled at build time. The refusals for the six radio entry points
// come from components/zigbee_mgr/radio_stubs.cpp instead.
#include "esp_log.h"
bool esp_zigbee_backend_register(void) {
    ESP_LOGW("esp_zb", "built without CONFIG_ZHAC_ESP_ZIGBEE -- no radio backend");
    return false;
}

#endif  // CONFIG_ZHAC_ESP_ZIGBEE
