// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hub speaker: the codec probe, the I2S stream, the amplifier pin, NVS, the rule hooks and the render
// task around the pure player of sound_synth.h, and the Hub Sound device (a PROTO_LOCAL pool entry whose
// `warning` the alarm sends as a siren output). Locking: s_mu guards s_player and s_save around every player
// call, render included; callers get the player's answer at once. Only the task writes NVS (volume and mute,
// between sounds, outside the lock): callers may run on a PSRAM stack, and one writer keeps the saved values
// equal to the live ones. The pool lock (recursive, and a device_cmd caller may hold it) is never taken with
// s_mu held: the device's last_seen is touched before s_mu is taken or after it is released.
#include "sound.h"

#include "sdkconfig.h"

#if CONFIG_ZHAC_AUDIO

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "device_shadow.h"
#include "es8311.h"
#include "local_dev.h"
#include "simple_rules.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zigbee_pool.h"

namespace {

constexpr const char* TAG             = "sound";
constexpr const char* kNs             = "sound";
constexpr size_t      kBlock          = 256;                   // samples per render and per DMA buffer: 16 ms
constexpr uint32_t    kLingerBlocks   = kSoundRate / kBlock;   // ~1 s of silence before the amplifier goes off
constexpr uint32_t    kWriteTimeoutMs = 100;
constexpr int         kProbeTries     = 3;                     // chip-ID asks before the board counts as speakerless
constexpr uint32_t    kProbeGapMs     = 20;
constexpr uint32_t    kIdleWakeMs     = 60000;                 // the idle task still wakes this often
constexpr int64_t     kSeenEveryUs    = 55LL * 1000000;        // under kIdleWakeMs: an idle wake always passes it
constexpr gpio_num_t  kPa             = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_PA_GPIO);
constexpr size_t      kLeadBlocks     =                        // silence while the amplifier wakes, whole blocks
    (CONFIG_ZHAC_AUDIO_PA_LEAD_MS * kSoundRate / 1000 + kBlock - 1) / kBlock;
constexpr int16_t     kSilence[kBlock] = {};
static_assert(kSoundRate == 16000, "es8311.cpp holds the clock row for 16 kHz only");

constexpr int kPins[] = {
    CONFIG_ZHAC_AUDIO_I2C_SDA_GPIO, CONFIG_ZHAC_AUDIO_I2C_SCL_GPIO, CONFIG_ZHAC_AUDIO_MCLK_GPIO,
    CONFIG_ZHAC_AUDIO_BCLK_GPIO,    CONFIG_ZHAC_AUDIO_LRCK_GPIO,    CONFIG_ZHAC_AUDIO_DOUT_GPIO,
    CONFIG_ZHAC_AUDIO_ASDOUT_GPIO,  CONFIG_ZHAC_AUDIO_PA_GPIO,
};

SemaphoreHandle_t        s_mu = nullptr;
SoundPlayer              s_player{};
bool                     s_present = false;   // set in app_main before any task that calls in exists
std::atomic<const char*> s_err{""};           // set once, when the start fails
char                     s_err_buf[64];
i2c_master_bus_handle_t  s_bus  = nullptr;
i2c_master_dev_handle_t  s_dev  = nullptr;
i2s_chan_handle_t        s_tx   = nullptr;
TaskHandle_t             s_task = nullptr;
bool                     s_save = false;      // under s_mu: volume or mute changed since the task saved them
int16_t                  s_buf[kBlock];       // the task's block: static, off its stack

struct Lock {
    Lock()  { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};

// ── NVS: `vol` and `muted`, as sys_state.cpp keeps its flags ──

uint8_t load_u8(const char* key, uint8_t def) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return def;
    uint8_t v = def;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v;
}

// The sound task's only: `vol` and `muted` as they are now, when a setter changed either since the last
// save. NVS skips a value it already holds.
void save_settings() {
    uint8_t vol, muted;
    {
        Lock l;
        if (!s_save) return;
        s_save = false;
        vol    = static_cast<uint8_t>(s_player.volume);
        muted  = s_player.muted ? 1 : 0;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(kNs, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, "vol", vol);
        if (e == ESP_OK) e = nvs_set_u8(h, "muted", muted);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "volume and mute not saved: %s", esp_err_to_name(e));
}

// ── The Hub Sound device's last_seen ──

uint32_t wall_s() {
    const time_t t = time(nullptr);
    return t > 1577836800 ? static_cast<uint32_t>(t) : 0;
}

void touch_last_seen() {   // the speaker is always there; never called with s_mu held
    const uint32_t t = wall_s();
    if (!t) return;
    zigbee_pool_lock();
    if (ZapDevice* d = pool_find_by_ieee(kHubSoundIeee)) d->last_seen = t;
    zigbee_pool_unlock();
}

// ── Boot ──

// The amplifier's enable pin: low, then an output. A floating enable line can hiss or pop a board without a
// pull-down, so sound_early_init does this as soon as the codec answers, and start() again before the stream.
esp_err_t pa_low() {
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << CONFIG_ZHAC_AUDIO_PA_GPIO;
    pa.mode         = GPIO_MODE_OUTPUT;
    if (const esp_err_t e = gpio_set_level(kPa, 0); e != ESP_OK) return e;
    return gpio_config(&pa);
}

// The I2C bus and the chip-ID check. One missed answer must not make the board speakerless (that removes the
// stored Hub Sound device and its name and room), so the chip ID is asked up to kProbeTries times, kProbeGapMs
// apart, on the one bus. es8311_probe leaves no device handle on the bus when it fails, so a retry adds none twice.
// No codec: the bus goes and SDA/SCL are reset, as before.
bool probe() {
    i2c_master_bus_config_t bc = {};
    bc.i2c_port          = -1;   // any free port
    bc.sda_io_num        = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_I2C_SDA_GPIO);
    bc.scl_io_num        = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_I2C_SCL_GPIO);
    bc.clk_source        = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bc, &s_bus) == ESP_OK) {
        for (int i = 0; i < kProbeTries; i++) {
            if (i) vTaskDelay(pdMS_TO_TICKS(kProbeGapMs));
            if (es8311_probe(s_bus, &s_dev)) {
                ESP_LOGI(TAG, "ES8311 found");
                return true;
            }
        }
    }
    if (s_bus) i2c_del_master_bus(s_bus);
    s_bus = nullptr;
    gpio_reset_pin(bc.sda_io_num);
    gpio_reset_pin(bc.scl_io_num);
    ESP_LOGI(TAG, "no ES8311 at 0x18, audio off");
    return false;
}

// ── The render task ──

// One block to the I2S queue. It waits while all four DMA buffers are full (16 ms each); the first failure
// is logged.
void write_block(const int16_t* b, bool* warned) {
    size_t done = 0;
    const esp_err_t e = i2s_channel_write(s_tx, b, kBlock * sizeof(b[0]), &done, kWriteTimeoutMs);
    if (e != ESP_OK && !*warned) {
        *warned = true;
        ESP_LOGW(TAG, "I2S write: %s", esp_err_to_name(e));
    }
}

// Plays the player block by block. A sound that starts while the amplifier is off switches it on and first
// plays CONFIG_ZHAC_AUDIO_PA_LEAD_MS of silence: the amplifier mutes its output while it wakes, and the first
// ~50-100 ms of a sound were lost (a whole `beep`, by ear on the P4-NANO kit). It goes off after ~1 s of
// silence; a sound in that second keeps it on and starts at once. Idle with it off, the task saves a changed
// volume or mute, then sleeps until a caller's notify, a minute at most. The I2S channel runs throughout and
// sends zeros whenever no block is queued. Every pass, whatever plays, also keeps the Hub Sound device's
// last_seen current (kSeenEveryUs): a loop can keep the idle wait away for 10 min.
void sound_task(void*) {
    bool     amp = false;
    uint32_t quiet = 0;          // silent blocks written since the last sound
    unsigned stack_free = ~0u;   // the least free stack logged so far
    bool     warned = false;
    int64_t  next_seen_us = 0;
    for (;;) {
        if (const int64_t now = esp_timer_get_time(); now >= next_seen_us) {
            next_seen_us = now + kSeenEveryUs;
            touch_last_seen();   // before the lock below; the PA, the linger and the I2S writes are not involved
        }
        size_t n = 0;
        bool   wake;
        {
            Lock l;
            // With the amplifier off, a sound waits unrendered while it wakes, so a stop, a mute or a
            // replace during the lead still counts.
            wake = !amp && sound_player_playing(&s_player) != nullptr;
            if (!wake) n = sound_player_render(&s_player, s_buf, kBlock);   // below kBlock: the sound ended, idle after
        }
        if (wake) {
            gpio_set_level(kPa, 1);
            amp = true;
            for (size_t i = 0; i < kLeadBlocks; i++) write_block(kSilence, &warned);
            continue;
        }
        if (n) {
            quiet = 0;
        } else if (!amp) {
            save_settings();   // a setter's wake ends here: the amplifier stays off, no linger
            if (const unsigned f = uxTaskGetStackHighWaterMark(nullptr); f < stack_free) {   // for the hardware check
                stack_free = f;
                ESP_LOGI(TAG, "task: %u B of stack never used", f);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleWakeMs));
            continue;
        } else if (++quiet > kLingerBlocks) {
            gpio_set_level(kPa, 0);
            amp = false;
            continue;
        }
        std::memset(s_buf + n, 0, (kBlock - n) * sizeof(s_buf[0]));
        write_block(s_buf, &warned);
    }
}

// The amplifier pin (low), I2S, the codec, the task. nullptr when all of it runs, else the step that failed
// (why in *e); a failure after the I2S start stops the stream again.
const char* start(esp_err_t* e) {
    if ((*e = pa_low()) != ESP_OK) return "amplifier pin";

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num  = 4;
    cc.dma_frame_num = kBlock;
    cc.auto_clear    = true;
    if ((*e = i2s_new_channel(&cc, &s_tx, nullptr)) != ESP_OK) return "I2S channel";
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kSoundRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_MCLK_GPIO),
            .bclk = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_BCLK_GPIO),
            .ws   = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_LRCK_GPIO),
            .dout = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_DOUT_GPIO),
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // MCLK 4.096 MHz: es8311.cpp's clock row
    if ((*e = i2s_channel_init_std_mode(s_tx, &sc)) != ESP_OK) return "I2S setup";
    if ((*e = i2s_channel_enable(s_tx)) != ESP_OK) return "I2S start";

    // The task's stack: 6 KB like the other tasks that commit NVS (the commit path uses ~3 KB). Internal RAM
    // (xTaskCreate): it writes flash, so its stack cannot sit in PSRAM.
    const char* step = nullptr;
    if (!es8311_dac_start(s_dev, e)) {
        step = "codec setup";
    } else if (xTaskCreate(sound_task, "sound", 6144, nullptr, 5, &s_task) != pdPASS) {
        *e   = ESP_ERR_NO_MEM;
        step = "render task";
    }
    if (step) i2s_channel_disable(s_tx);
    return step;
}

// ── Rule hooks ──

// The built-in names after the n bytes already in err, comma-joined; only the loopable ones if asked.
void list_names(char* err, size_t cap, size_t n, bool loopable) {
    const char* sep = "";
    for (size_t i = 0; i < sound_count() && n < cap; i++) {
        if (loopable && !sound_loopable(i)) continue;
        n += static_cast<size_t>(std::snprintf(err + n, cap - n, "%s%s", sep, sound_name(i)));
        sep = ",";
    }
}

bool rules_check(const char* name, bool loop, char* err, size_t cap) {
    if (!s_present) {
        std::snprintf(err, cap, "%s", sound_result_text(SoundResult::ABSENT));
        return false;
    }
    if (!name || !*name) return true;   // a bare sound.stop
    const int i = sound_find(name);
    if (i >= 0 && (!loop || sound_loopable(static_cast<size_t>(i)))) return true;
    const int n = i < 0 ? std::snprintf(err, cap, "unknown sound, use: ")
                        : std::snprintf(err, cap, "%s cannot loop, loopable: ", name);
    list_names(err, cap, static_cast<size_t>(n), i >= 0);
    return false;
}

bool rules_run(bool play, const char* name, int volume, bool loop) {
    if (!play) {
        sound_stop(name);
        return true;
    }
    const SoundResult r = sound_play(name, volume, loop);
    if (r == SoundResult::OK || r == SoundResult::MUTED) return true;
    ESP_LOGW(TAG, "rule: sound.play %s: %s", name, sound_result_text(r));
    return false;
}

// ── Callers ──

// The player's answer to `f`, under the lock; ABSENT with no codec, or one that did not start. An OK wakes
// the task (before sound_start there is none yet: the task looks at the player before it first sleeps).
template <typename F>
SoundResult admit(F&& f) {
    if (!s_present) return SoundResult::ABSENT;
    SoundResult r;
    {
        Lock l;
        if (*s_err.load()) return SoundResult::ABSENT;
        r = f();
    }
    if (r == SoundResult::OK && s_task) xTaskNotifyGive(s_task);
    return r;
}

// ── The Hub Sound device ──

// One input, `warning` (write-only): a Bool or an integer is a siren for 10 s or n s (false and 0 stop it), a
// string the IAS WD text of sound_warning_text. It plays as DEVICE, so a stop ends only the device's own sound.
DevCmdResult local_cmd(uint64_t ieee, const char* key, const DevCmdValue* v) {
    if (ieee != kHubSoundIeee) return DEVCMD_NOT_FOUND;
    if (!*s_err.load()) touch_last_seen();   // a speaker that failed to start is not "there"
    if (std::strcmp(key, "warning") != 0) return DEVCMD_NO_CONVERTER;
    SoundWarning w;
    switch (v->kind) {
    case DEVCMD_BOOL: w = sound_warning_secs(v->b ? 10 : 0); break;
    case DEVCMD_INT:
        if (v->i < 0) return DEVCMD_BAD_VALUE;
        w = sound_warning_secs(static_cast<uint32_t>(std::min<int64_t>(v->i, UINT32_MAX)));   // held to 600 there
        break;
    case DEVCMD_STR:
        if (!v->s || !sound_warning_text(v->s, &w)) return DEVCMD_BAD_VALUE;
        break;
    default: return DEVCMD_BAD_VALUE;
    }
    return admit([&] { return sound_player_warn(&s_player, w); }) == SoundResult::OK ? DEVCMD_OK : DEVCMD_BAD_VALUE;
}

size_t local_exposes(uint64_t ieee, char* buf, size_t cap) {
    if (ieee != kHubSoundIeee || cap < sizeof(kHubSoundExposes)) return 0;
    std::memcpy(buf, kHubSoundExposes, sizeof(kHubSoundExposes));
    return sizeof(kHubSoundExposes) - 1;
}

}  // namespace

void sound_early_init() {
    s_mu = xSemaphoreCreateMutex();
    configASSERT(s_mu);
    sound_player_init(&s_player, CONFIG_ZHAC_AUDIO_MAX_VOLUME);
    sound_player_set_volume(&s_player, load_u8("vol", static_cast<uint8_t>(s_player.volume)));
    sound_player_set_muted(&s_player, load_u8("muted", 0) != 0);
    s_present = probe();
    if (s_present) {
        pa_low();   // the codec answered, so the pin is ours; a failure shows again in start(), which reports it
        local_dev_register(kHubSoundIeee, local_cmd, local_exposes);   // before any command can come
    }
    simple_rules_set_sound_hooks(rules_check, rules_run);   // with or without a codec: the check says which
}

void sound_start() {
    if (!s_present) return;
    esp_err_t e = ESP_OK;
    const char* step = start(&e);
    if (!step) {
        ESP_LOGI(TAG, "speaker on: volume %d of %d%s", sound_volume(), sound_max_volume(), sound_muted() ? ", muted" : "");
        return;
    }
    // Off for this boot: no stream, the amplifier pin low (if it got that far), every play ABSENT.
    if (s_tx) i2s_del_channel(s_tx);
    s_tx = nullptr;
    std::snprintf(s_err_buf, sizeof(s_err_buf), "%s failed: %s", step, esp_err_to_name(e));
    {
        Lock l;
        s_err.store(s_err_buf);
        sound_player_stop(&s_player, nullptr);   // a sound asked for before the start never plays
    }
    ESP_LOGE(TAG, "%s: audio off", s_err_buf);
}

bool        sound_present() { return s_present; }
const char* sound_error() { return s_err.load(); }

bool sound_owns_pin(int gpio) {
    if (!s_present) return false;
    for (int p : kPins) if (p == gpio) return true;
    return false;
}

// Mirrors alarm_attach_device. No event at boot; a board without the speaker drops a stored entry, with none either.
void sound_attach_device() {
    if (!s_present) {
        bool removed = false;
        zigbee_pool_lock();
        if (const ZapDevice* d = pool_find_by_ieee(kHubSoundIeee)) {
            pool_remove(static_cast<uint16_t>(d - pool_all()));
            removed = true;
        }
        zigbee_pool_unlock();
        if (removed) {
            zap_store_delete_device(kHubSoundIeee);
            device_shadow_remove(kHubSoundIeee);
            ESP_LOGI(TAG, "no speaker on this board: the stored Hub Sound device is removed");
        }
        return;
    }
    // PSRAM, not app_main's tight stack: copies of the pool's entry, and zap_store writes from its own copy.
    auto* m = static_cast<ZapDevice*>(heap_caps_malloc(2 * sizeof(ZapDevice), MALLOC_CAP_SPIRAM));
    if (!m) {
        ESP_LOGE(TAG, "out of memory: the Hub Sound device is not shown");
        return;
    }
    ZapDevice& snap   = m[0];
    ZapDevice& before = m[1];
    std::memset(&snap, 0, sizeof(snap));
    bool save = false;
    zigbee_pool_lock();
    ZapDevice* d = pool_find_by_ieee(kHubSoundIeee);
    bool added = false;
    if (!d && (d = pool_add()) != nullptr) {
        d->ieee_addr = kHubSoundIeee;
        std::snprintf(d->friendly_name, sizeof(d->friendly_name), "Hub Sound");
        added = true;
    }
    if (d) {
        before = *d;
        d->protocol        = PROTO_LOCAL;
        d->nwk_addr        = 0xFFFE;   // "no network address"
        d->device_type     = 1;        // mains powered, always there
        d->power_source    = 1;
        d->flags          &= static_cast<uint8_t>(~(ZAP_DEV_REMOVED | ZAP_DEV_FIX_BINDS | ZAP_DEV_FIX_CIE));
        d->interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_READY);
        d->support_state   = static_cast<uint8_t>(SupportState::MATCHED);
        d->configure_state = static_cast<uint8_t>(ConfigureState::DONE);
        std::snprintf(d->model_id, sizeof(d->model_id), "ZHAC-SOUND");
        std::snprintf(d->manufacturer_name, sizeof(d->manufacturer_name), "ZHAC");
        if (const uint32_t t = wall_s()) d->last_seen = t;
        save = added || std::memcmp(before.model_id, d->model_id, sizeof(d->model_id)) != 0 ||
               before.protocol != d->protocol || before.flags != d->flags ||
               before.support_state != d->support_state || before.interview_state != d->interview_state ||
               before.configure_state != d->configure_state;
        snap = *d;
    }
    zigbee_pool_unlock();
    if (!snap.ieee_addr) ESP_LOGE(TAG, "device pool full: the Hub Sound device is not shown");
    if (save && !zap_store_save_device(&snap)) ESP_LOGW(TAG, "Hub Sound device not saved");
    heap_caps_free(m);
}

SoundResult sound_play(const char* name, int volume, bool loop) {
    const int idx = sound_find(name);
    return admit([&] { return sound_player_play(&s_player, idx, volume, loop, 0, SoundOwner::USER); });
}

SoundResult sound_tone(int hz, int ms, int volume) {
    return admit([&] { return sound_player_tone(&s_player, hz, ms, volume); });
}

SoundResult sound_melody(const SoundNote* notes, size_t n, int volume) {
    return admit([&] { return sound_player_melody(&s_player, notes, n, volume); });
}

bool sound_stop(const char* name) {
    Lock l;
    return sound_player_stop(&s_player, name);
}

const char* sound_playing() {
    Lock l;
    return sound_player_playing(&s_player);
}

int sound_volume() {
    Lock l;
    return s_player.volume;
}

int sound_set_volume(int v) {
    int          now;
    TaskHandle_t t;
    {
        Lock l;
        now    = sound_player_set_volume(&s_player, v);
        s_save = true;
        t      = s_task;
    }
    if (t) xTaskNotifyGive(t);   // the task saves it
    return now;
}

bool sound_muted() {
    Lock l;
    return s_player.muted;
}

void sound_set_muted(bool m) {
    TaskHandle_t t;
    {
        Lock l;
        sound_player_set_muted(&s_player, m);
        s_save = true;
        t      = s_task;
    }
    if (t) xTaskNotifyGive(t);   // the task saves it
}

int sound_max_volume() { return s_player.max_volume; }   // fixed in sound_early_init

#else

void        sound_early_init() {}
void        sound_start() {}
bool        sound_present() { return false; }
const char* sound_error() { return ""; }
bool        sound_owns_pin(int) { return false; }
void        sound_attach_device() {}
SoundResult sound_play(const char*, int, bool) { return SoundResult::ABSENT; }
SoundResult sound_tone(int, int, int) { return SoundResult::ABSENT; }
SoundResult sound_melody(const SoundNote*, size_t, int) { return SoundResult::ABSENT; }
bool        sound_stop(const char*) { return false; }
const char* sound_playing() { return nullptr; }
int         sound_volume() { return 0; }
int         sound_set_volume(int) { return 0; }
bool        sound_muted() { return false; }
void        sound_set_muted(bool) {}
int         sound_max_volume() { return 0; }

#endif
