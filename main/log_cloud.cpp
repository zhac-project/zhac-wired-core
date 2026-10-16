// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// "Log to cloud" (spec 2026-10-06 cloud logs §4).
//
// The log hook calls log_cloud_capture() for every line. It copies the line into the PSRAM backlog when the
// hub's level lets it through, and always into the restart tail: a 4 KB ring in no-init RAM that the next run
// of the same firmware reads. A low-priority task sends the backlog as log.batch events. Once per run it sends
// log.tail: the reset reason, a fresh crash's summary, and the previous run's lines the cloud never got.
//
// Rules this file keeps:
// - The capture only copies under a spinlock: it never sends, logs, allocates or blocks.
// - The uploader's stack may be in PSRAM (zhac_task_create on the S31), so it never touches NVS or flash.
//   The core dump and its NVS marker are read in log_cloud_boot(), on the main task.
// - This file's own tag, "log_cloud", is on the link-tag list: its lines never feed the upload.
#include "log_cloud.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_IDF_TARGET_ARCH_RISCV
#include "esp_core_dump.h"
#include "esp_flash.h"
#define LOG_CLOUD_CRASH_SUMMARY 1
#endif
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "log_cloud_boot() tells an update's first boot by the OTA trial state (ESP_OTA_IMG_PENDING_VERIFY)"
#endif

#include "ota_update.h"
#include "remote_client.h"
#include "remote_state.h"
#include "sys_state.h"
#include "zap_clock.h"
#include "zhac_task.h"

using namespace logcloud;

namespace {

const char* TAG = "log_cloud";

constexpr uint32_t kTailMagic    = 0x5A4C5431;   // "ZLT1"
constexpr uint32_t kBacklogMagic = 0x5A4C4231;   // "ZLB1"
constexpr size_t   kTailBytes    = 4096;
constexpr size_t   kBacklogBytes = 64 * 1024;
// [[maybe_unused]]: only the uploader (compiled with the cloud link) reads these.
[[maybe_unused]] constexpr size_t   kBatchBytes  = BatchWriter::kBudget;
[[maybe_unused]] constexpr int64_t  kSendEveryUs = 5LL * 1000 * 1000;
[[maybe_unused]] constexpr uint32_t kStackBytes  = 6144;

// Survives a software restart (panic, watchdog, esp_restart); garbage after power-on, which Ring::valid catches.
alignas(8) __NOINIT_ATTR uint8_t s_tail_block[kTailBytes];

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // guards both rings and s_seq
Ring     s_tail(s_tail_block, kTailBytes, /*sealed=*/true);   // the next run checks it, so its crc is kept
Ring*    s_backlog = nullptr;          // unsealed: it never outlives the run, so push/pop skip the crc
uint32_t s_seq     = 0;                // the last backlog sequence number given out

// The previous run, as found at boot.
uint8_t* s_prev_block = nullptr;       // PSRAM copy of its tail; nullptr when none survived
TailHead s_prev{};                     // prev_boot, prev_sent, prev_clock_base_ms, planned
Reset    s_reason = Reset::Other;
char     s_crash[200] = "";            // crash_json() of a fresh core dump, or ""
[[maybe_unused]] bool s_tail_due = true;   // log.tail not sent yet in this run

std::atomic<uint32_t> s_sent{0};       // lines handed to the cloud link since start
std::atomic<uint32_t> s_dropped{0};    // backlog drops already reported in a batch
[[maybe_unused]] TaskHandle_t s_task = nullptr;

Reset map_reset(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:  return Reset::PowerOn;
    case ESP_RST_PANIC:    return Reset::Panic;
    case ESP_RST_INT_WDT:  return Reset::IntWdt;
    case ESP_RST_TASK_WDT: return Reset::TaskWdt;
    case ESP_RST_WDT:      return Reset::Wdt;
    case ESP_RST_BROWNOUT: return Reset::Brownout;
    case ESP_RST_SW:       return Reset::Sw;
    case ESP_RST_EXT:      return Reset::Ext;
    default:               return Reset::Other;
    }
}

[[maybe_unused]] int64_t clock_ms() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return zap_clock_is_set(tv.tv_sec) ? int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000 : 0;
}

#ifdef LOG_CLOUD_CRASH_SUMMARY
// A fresh dump exists after a panic or an interrupt watchdog (both go through the panic handler). It is
// reported once: NVS sys_cfg/cd_sent holds the first 4 bytes of the reported dump's SHA-256 (the image's last
// 32 bytes). An older dump that a later crash failed to replace has the same id, so it is not reported twice.
void read_crash() {
    if (s_reason != Reset::Panic && s_reason != Reset::IntWdt) return;
    if (esp_core_dump_image_check() != ESP_OK) return;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size < 32) return;
    uint32_t id = 0;
    if (esp_flash_read(nullptr, &id, addr + size - 32, sizeof(id)) != ESP_OK) return;
    if (id == 0) id = 1;
    nvs_handle_t h;
    if (nvs_open("sys_cfg", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t seen = 0;
    nvs_get_u32(h, "cd_sent", &seen);
    if (seen != id) {
        auto* sum = static_cast<esp_core_dump_summary_t*>(
            heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
            char why[96] = "";
            if (esp_core_dump_get_panic_reason(why, sizeof(why)) != ESP_OK || !why[0])
                std::snprintf(why, sizeof(why), "%s", riscv_cause_name(sum->ex_info.mcause));
            crash_json(s_crash, sizeof(s_crash), sum->exc_task, sum->exc_pc, why,
                       reinterpret_cast<const char*>(sum->app_elf_sha256));
            if (nvs_set_u32(h, "cd_sent", id) == ESP_OK) nvs_commit(h);
        }
        heap_caps_free(sum);
    }
    nvs_close(h);
}
#endif

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
bool link_ready() {
    RemoteStatusSnap s{};
    remote_client_get_status(&s);
    return s.state == REMOTE_STATE_READY && remote_client_tx_free() >= CONFIG_ZHAC_REMOTE_TX_QUEUE_DEPTH / 2;
}

void send_tail(char* out) {
    TailHead h = s_prev;
    h.boot       = remote_client_boot_epoch();
    h.up_ms      = static_cast<uint64_t>(esp_timer_get_time()) / 1000u;
    h.clock_ms   = clock_ms();
    h.reason     = reset_name(s_reason);
    h.crash_json = s_crash[0] ? s_crash : nullptr;
    size_t n = 0;
    if (s_prev_block) {
        const Ring prev(s_prev_block, kTailBytes);
        n = tail_json(out, kBatchBytes, h, &prev);
    } else {
        n = tail_json(out, kBatchBytes, h, nullptr);
    }
    if (n) remote_client_publish_event("log.tail", out, n);
    heap_caps_free(s_prev_block);
    s_prev_block = nullptr;
}

size_t backlog_used() {
    portENTER_CRITICAL(&s_mux);
    const size_t u = s_backlog->used();
    portEXIT_CRITICAL(&s_mux);
    return u;
}

// Moves the oldest backlog lines into one log.batch and hands it to the link. false = the backlog was empty.
// The lines of a batch are consecutive (the cloud reads `seq` as the first and the others in order): a line
// the hook dropped meanwhile ends the batch, and the next one starts after the gap with its dropped count.
bool send_batch(char* out, BatchWriter& bw) {
    Line l;
    uint32_t dropped = 0;
    portENTER_CRITICAL(&s_mux);
    bool have = s_backlog->peek(&l);
    if (have) dropped = s_backlog->take_dropped();
    portEXIT_CRITICAL(&s_mux);
    if (!have) return false;
    bw.begin(remote_client_boot_epoch(), l.seq, static_cast<uint64_t>(esp_timer_get_time()) / 1000u, clock_ms(),
             dropped);
    uint32_t last = 0;
    while (have && bw.add(l)) {
        last = l.seq;
        portENTER_CRITICAL(&s_mux);
        // Not at the front any more: the hook dropped it after it was copied. It is sent, so it is not lost.
        if (!s_backlog->pop_if(last) && s_backlog->hdr().dropped) s_backlog->hdr().dropped--;
        have = s_backlog->peek(&l) && l.seq == last + 1;
        portEXIT_CRITICAL(&s_mux);
    }
    const size_t n = bw.finish();
    if (n == 0) {                       // the first line did not fit: impossible with the caps, never stall on it
        portENTER_CRITICAL(&s_mux);
        s_backlog->pop_if(l.seq);
        portEXIT_CRITICAL(&s_mux);
        return true;
    }
    remote_client_publish_event("log.batch", out, n);
    s_sent += static_cast<uint32_t>(bw.lines());
    s_dropped += dropped;
    portENTER_CRITICAL(&s_mux);
    s_tail.hdr().sent = last;           // the next run's tail leaves 1..last out
    s_tail.seal();
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void uploader(void*) {
    char* out = static_cast<char*>(heap_caps_malloc(kBatchBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out || !s_backlog) {
        ESP_LOGE(TAG, "no memory -- no cloud logs this run");
        for (;;) vTaskDelay(portMAX_DELAY);    // a task with a PSRAM stack must not delete itself
    }
    BatchWriter bw(out, kBatchBytes);
    int64_t last_send = esp_timer_get_time();
    bool stack_logged = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now  = esp_timer_get_time();
        const int64_t wall = clock_ms();
        const bool    off  = level_from_u8(sys_log_cloud()) == Level::Off;
        portENTER_CRITICAL(&s_mux);
        if (wall) { s_tail.hdr().clock_base_ms = wall - now / 1000; s_tail.seal(); }
        if (off && s_backlog->count()) s_backlog->clear();
        portEXIT_CRITICAL(&s_mux);
        if (off || !link_ready()) continue;
        if (s_tail_due) { send_tail(out); s_tail_due = false; continue; }
        const size_t pending = backlog_used();
        if (pending == 0) continue;
        if (pending < kBatchBytes && now - last_send < kSendEveryUs) continue;
        do {
            if (!send_batch(out, bw)) break;
            last_send = esp_timer_get_time();
            if (!stack_logged) {   // spec §8: the deepest paths (log.tail, a full batch) have run by now
                stack_logged = true;
                ESP_LOGI(TAG, "first batch sent (stack: %u B never used)",
                         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            }
        } while (link_ready() && backlog_used() >= kBatchBytes);
    }
}
#endif

}  // namespace

void log_cloud_boot() {
    s_reason = map_reset(esp_reset_reason());
    // The tail block sits after this image's own code and data, so another image puts it elsewhere (spec §4.4
    // Limits). The header's image id keeps a block another image left from being read as ours, and update /
    // rollback come from the OTA trial state and the hub's rollback flag (prev_run).
    uint32_t image = 0;
    std::memcpy(&image, esp_app_get_description()->app_elf_sha256, sizeof(image));
    esp_ota_img_states_t ota = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const bool on_trial =
        running && esp_ota_get_state_partition(running, &ota) == ESP_OK && ota == ESP_OTA_IMG_PENDING_VERIFY;
    const bool valid = s_tail.valid(kTailMagic, image);
    const PrevRun prev =
        prev_run(valid, valid ? s_tail.hdr().planned : 0, on_trial, s_reason, ota_update_rolled_back());
    if (prev.use_tail) {
        const Ring::Header& ph = s_tail.hdr();
        s_prev.prev_boot          = ph.run_id;
        s_prev.prev_sent          = ph.sent;
        s_prev.prev_clock_base_ms = ph.clock_base_ms;
        s_prev_block = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(8, kTailBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_prev_block) std::memcpy(s_prev_block, s_tail_block, kTailBytes);
    }
    s_prev.planned = planned_name(prev.planned);
    s_tail.format(kTailMagic, remote_client_boot_epoch(), image);
#ifdef LOG_CLOUD_CRASH_SUMMARY
    read_crash();
#endif
    if (void* b = heap_caps_aligned_alloc(8, kBacklogBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        static Ring backlog(b, kBacklogBytes, /*sealed=*/false);
        backlog.format(kBacklogMagic, remote_client_boot_epoch());
        s_backlog = &backlog;
    }
    ESP_LOGI(TAG, "boot: reset=%s planned=%s tail=%s crash=%s backlog=%s", reset_name(s_reason),
             s_prev.planned ? s_prev.planned : "-", s_prev_block ? "kept" : "none", s_crash[0] ? "yes" : "no",
             s_backlog ? "64 KB" : "NO MEMORY");
}

void log_cloud_capture(char level, const char* tag, const char* msg, int64_t up_us) {
    if (level != 'E' && level != 'W' && level != 'I') return;
    const uint64_t up_ms   = static_cast<uint64_t>(up_us) / 1000u;
    const Level    setting = level_from_u8(sys_log_cloud());
    portENTER_CRITICAL(&s_mux);
    uint32_t seq = 0;
    if (s_backlog && wants(setting, level, tag)) {
        seq = ++s_seq;
        s_backlog->push(seq, up_ms, level, tag, msg);
    }
    s_tail.push(seq, up_ms, level, tag, msg);
    portEXIT_CRITICAL(&s_mux);
}

void log_cloud_start() {
#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    if (s_task) return;
    if (zhac_task_create(uploader, "log_cloud", kStackBytes, nullptr, tskIDLE_PRIORITY + 1, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "task start failed -- no cloud logs");
    }
#endif
}

void log_cloud_on_setting() {
    if (level_from_u8(sys_log_cloud()) != Level::Off) return;
    portENTER_CRITICAL(&s_mux);
    if (s_backlog) s_backlog->clear();
    portEXIT_CRITICAL(&s_mux);
}

void log_cloud_planned_restart(Planned p) {
    portENTER_CRITICAL(&s_mux);
    s_tail.hdr().planned = static_cast<uint32_t>(p);
    s_tail.seal();
    portEXIT_CRITICAL(&s_mux);
}

void log_cloud_stats(uint32_t* sent, uint32_t* dropped) {
    uint32_t pending = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_backlog) pending = s_backlog->hdr().dropped;
    portEXIT_CRITICAL(&s_mux);
    if (sent) *sent = s_sent.load();
    if (dropped) *dropped = s_dropped.load() + pending;
}
