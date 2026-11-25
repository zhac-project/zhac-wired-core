// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hub speaker: the codec probe, the I2S stream, the amplifier pin, NVS, the rule hooks and the render
// task around the pure player of sound_synth.h. Locking: s_mu guards s_player around every player call,
// render included; callers get the player's answer at once and the task only renders. NVS is written
// outside the lock, so a save never stalls the stream.
#include "sound.h"

#include "sdkconfig.h"

#if CONFIG_ZHAC_AUDIO

#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "es8311.h"
#include "simple_rules.h"

namespace {

constexpr const char* TAG             = "sound";
constexpr const char* kNs             = "sound";
constexpr size_t      kBlock          = 256;                   // samples per render and per DMA buffer: 16 ms
constexpr uint32_t    kLingerBlocks   = kSoundRate / kBlock;   // ~1 s of silence before the amplifier goes off
constexpr uint32_t    kWriteTimeoutMs = 100;
constexpr uint32_t    kIdleWakeMs     = 60000;                 // the idle task still wakes this often
constexpr gpio_num_t  kPa             = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_PA_GPIO);
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

void save_u8(const char* key, uint8_t v) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(kNs, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, key, v);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "%s not saved: %s", key, esp_err_to_name(e));
}

// ── Boot ──

// The I2C bus and the chip-ID check. No codec: the bus goes and SDA/SCL are reset, as before.
bool probe() {
    i2c_master_bus_config_t bc = {};
    bc.i2c_port          = -1;   // any free port
    bc.sda_io_num        = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_I2C_SDA_GPIO);
    bc.scl_io_num        = static_cast<gpio_num_t>(CONFIG_ZHAC_AUDIO_I2C_SCL_GPIO);
    bc.clk_source        = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bc, &s_bus) == ESP_OK && es8311_probe(s_bus, &s_dev)) {
        ESP_LOGI(TAG, "ES8311 found");
        return true;
    }
    if (s_bus) i2c_del_master_bus(s_bus);
    s_bus = nullptr;
    gpio_reset_pin(bc.sda_io_num);
    gpio_reset_pin(bc.scl_io_num);
    ESP_LOGI(TAG, "no ES8311 at 0x18, audio off");
    return false;
}

// ── The render task ──

// Plays the player block by block. The amplifier goes on once a sound's first block is queued, and off after
// ~1 s of silence; a sound in that second keeps it on. Idle with it off, the task sleeps until a caller's
// notify, a minute at most. The I2S channel runs throughout and sends zeros whenever no block is queued.
void sound_task(void*) {
    bool     amp = false;
    uint32_t quiet = 0;   // silent blocks written since the last sound
    bool     warned = false, stack_logged = false;
    for (;;) {
        size_t n;
        {
            Lock l;
            n = sound_player_render(&s_player, s_buf, kBlock);   // below kBlock: the sound ended, idle after
        }
        if (n) {
            quiet = 0;
        } else if (!amp) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleWakeMs));
            continue;
        } else if (++quiet > kLingerBlocks) {
            gpio_set_level(kPa, 0);
            amp = false;
            if (!stack_logged) {   // for the hardware check: a whole sound has run through the task
                stack_logged = true;
                ESP_LOGI(TAG, "task: %u B of stack never used", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
            continue;
        }
        std::memset(s_buf + n, 0, (kBlock - n) * sizeof(s_buf[0]));
        size_t done = 0;
        const esp_err_t e = i2s_channel_write(s_tx, s_buf, sizeof(s_buf), &done, kWriteTimeoutMs);
        if (e != ESP_OK && !warned) {
            warned = true;
            ESP_LOGW(TAG, "I2S write: %s", esp_err_to_name(e));
        }
        if (n && !amp && e == ESP_OK) {
            gpio_set_level(kPa, 1);
            amp = true;
        }
    }
}

// The amplifier pin (low), I2S, the codec, the task. nullptr when all of it runs, else the step that failed
// (why in *e); a failure after the I2S start stops the stream again.
const char* start(esp_err_t* e) {
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << CONFIG_ZHAC_AUDIO_PA_GPIO;
    pa.mode         = GPIO_MODE_OUTPUT;
    if ((*e = gpio_set_level(kPa, 0)) != ESP_OK || (*e = gpio_config(&pa)) != ESP_OK) return "amplifier pin";

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

    const char* step = nullptr;
    if (!es8311_dac_start(s_dev, e)) {
        step = "codec setup";
    } else if (xTaskCreate(sound_task, "sound", 4096, nullptr, 5, &s_task) != pdPASS) {   // internal stack
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

}  // namespace

void sound_early_init() {
    s_mu = xSemaphoreCreateMutex();
    configASSERT(s_mu);
    sound_player_init(&s_player, CONFIG_ZHAC_AUDIO_MAX_VOLUME);
    sound_player_set_volume(&s_player, load_u8("vol", static_cast<uint8_t>(s_player.volume)));
    sound_player_set_muted(&s_player, load_u8("muted", 0) != 0);
    s_present = probe();
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
    int stored;
    {
        Lock l;
        stored = sound_player_set_volume(&s_player, v);
    }
    save_u8("vol", static_cast<uint8_t>(stored));
    return stored;
}

bool sound_muted() {
    Lock l;
    return s_player.muted;
}

void sound_set_muted(bool m) {
    {
        Lock l;
        sound_player_set_muted(&s_player, m);
    }
    save_u8("muted", m ? 1 : 0);
}

int sound_max_volume() { return s_player.max_volume; }   // fixed in sound_early_init

#else

void        sound_early_init() {}
void        sound_start() {}
bool        sound_present() { return false; }
const char* sound_error() { return ""; }
bool        sound_owns_pin(int) { return false; }
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
