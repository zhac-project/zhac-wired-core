// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "user_io.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "device_cmd.h"
#include "device_shadow.h"
#include "event_bus.h"
#include "simple_rules.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zcl_attribute.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"

namespace {

constexpr const char* TAG = "user_io";
constexpr const char* kNs = "user_io";
constexpr int64_t kLastSaveDelayUs = 3 * 1000 * 1000;   // `last` levels: one write per burst of changes
constexpr uint32_t kPollMs = 20;                         // inputs: 2 equal samples = 40 ms debounce
constexpr size_t kCfgJsonMax = 4096;
#if SOC_ADC_ATTEN_NUM > 1
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;   // 0 to ~3.1 V
#else
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_0;    // the only one there is (S31 on IDF 6.1-beta1)
#endif

struct PinRt {
    int32_t            value;      // out/in: 0|1, pwm: 0-100, adc: last published reading
    bool               have;       // in/adc: a value was read
    uint8_t            settle;     // in: samples the pin disagreed with `value`
    int8_t             ledc_ch;
    int8_t             ledc_tmr;
    uint8_t            ledc_bits;
    adc_unit_t         adc_unit;
    adc_channel_t      adc_chan;
    adc_cali_handle_t  cali;
    int64_t            next_adc_us;
    led_strip_handle_t strip;
    bool               ready;      // set up without error
    char               err[40];
};

struct Pub { char key[kUserIoNameMax]; ValType vt; int32_t v; };

struct LastRec { uint8_t gpio; uint8_t pad; int16_t value; };

SemaphoreHandle_t s_mu = nullptr;
UserIoCaps   s_caps{};
UserIoConfig s_cfg{};
PinRt        s_rt[kUserIoMaxPins]{};
LastRec      s_last[kUserIoMaxPins]{};
uint8_t      s_last_n = 0;
int64_t      s_last_dirty_us = 0;
TaskHandle_t s_task = nullptr;   // the poll task, created with the first pin
uint32_t     s_tmr_freq[SOC_LEDC_TIMER_NUM]{};
uint8_t      s_tmr_bits[SOC_LEDC_TIMER_NUM]{};
uint8_t      s_tmr_users[SOC_LEDC_TIMER_NUM]{};
bool         s_ch_used[SOC_LEDC_CHANNEL_NUM]{};
adc_oneshot_unit_handle_t s_adc[SOC_ADC_PERIPH_NUM]{};

struct Lock {
    Lock()  { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};

// Pins this build gives to something else, whatever ZHAC_IO_PINS says.
bool firmware_pin(int p) {
    static constexpr int kPins[] = {
#ifdef CONFIG_ZHAC_ETH_MDC_GPIO
        CONFIG_ZHAC_ETH_MDC_GPIO,
#endif
#ifdef CONFIG_ZHAC_ETH_MDIO_GPIO
        CONFIG_ZHAC_ETH_MDIO_GPIO,
#endif
#ifdef CONFIG_ZHAC_ETH_REF_CLK_GPIO
        CONFIG_ZHAC_ETH_REF_CLK_GPIO,
#endif
#ifdef CONFIG_ZHAC_ETH_PHY_PWR_GPIO
        CONFIG_ZHAC_ETH_PHY_PWR_GPIO,
#endif
#ifdef CONFIG_ZHAC_ETH_PHY_RST_GPIO
        CONFIG_ZHAC_ETH_PHY_RST_GPIO,
#endif
#ifdef CONFIG_ZHAC_RCP_UART_RX_GPIO
        CONFIG_ZHAC_RCP_UART_RX_GPIO,
#endif
#ifdef CONFIG_ZHAC_RCP_UART_TX_GPIO
        CONFIG_ZHAC_RCP_UART_TX_GPIO,
#endif
#ifdef CONFIG_ZHAC_RCP_RESET_GPIO
        CONFIG_ZHAC_RCP_RESET_GPIO,
#endif
#ifdef CONFIG_ZHAC_RCP_BOOT_GPIO
        CONFIG_ZHAC_RCP_BOOT_GPIO,
#endif
#ifdef CONFIG_ZHAC_STATUS_LED_GPIO
        CONFIG_ZHAC_STATUS_LED_GPIO,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_CLK
        CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_CLK,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_MOSI
        CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_MOSI,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_MISO
        CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_MISO,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_CS
        CONFIG_ESP_HOSTED_SPI_HSPI_GPIO_CS,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_GPIO_HANDSHAKE
        CONFIG_ESP_HOSTED_SPI_GPIO_HANDSHAKE,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_GPIO_DATA_READY
        CONFIG_ESP_HOSTED_SPI_GPIO_DATA_READY,
#endif
#ifdef CONFIG_ESP_HOSTED_SPI_GPIO_RESET_SLAVE
        CONFIG_ESP_HOSTED_SPI_GPIO_RESET_SLAVE,
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_TX_GPIO
        CONFIG_ESP_CONSOLE_UART_TX_GPIO,
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_RX_GPIO
        CONFIG_ESP_CONSOLE_UART_RX_GPIO,
#endif
        -1,
    };
    for (int q : kPins) if (q == p) return true;
    return false;
}

void build_caps() {
    uint8_t list[64];
    const uint8_t n = user_io_parse_pins(CONFIG_ZHAC_IO_PINS, list, sizeof(list));
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t p = list[i];
        if (!GPIO_IS_VALID_GPIO(p) || firmware_pin(p) || esp_gpio_is_reserved(1ULL << p)) {
            ESP_LOGW(TAG, "GPIO %u left out: it is not free on this build", p);
            continue;
        }
        bool dup = false;
        for (uint8_t j = 0; j < s_caps.n_allowed; j++) dup |= s_caps.allowed[j] == p;
        if (dup) continue;
        s_caps.allowed[s_caps.n_allowed++] = p;
        adc_unit_t u;
        adc_channel_t c;
        if (adc_oneshot_io_to_channel(p, &u, &c) == ESP_OK) s_caps.adc_mask |= 1ULL << p;
    }
}

// ── NVS: the config (as JSON, so a firmware update cannot misread it) and the `last` levels ──

bool nvs_put(const char* key, const void* data, size_t len) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = len ? nvs_set_blob(h, key, data, len) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

size_t nvs_get(const char* key, void* data, size_t cap) {
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_blob(h, key, data, &len) != ESP_OK) len = 0;
    nvs_close(h);
    return len;
}

size_t cfg_json(const UserIoConfig& c, char* buf, size_t cap) {
    size_t n = static_cast<size_t>(std::snprintf(buf, cap, "{\"pins\":["));
    for (uint8_t i = 0; i < c.count; i++) {
        if (i) { if (n + 1 >= cap) return 0; buf[n++] = ','; }
        const size_t m = user_io_cfg_pin_json(c.pins[i], buf + n, cap - n);
        if (!m) return 0;
        n += m;
    }
    if (n + 3 > cap) return 0;
    std::memcpy(buf + n, "]}", 3);
    return n + 2;
}

int32_t last_value(uint8_t gpio) {
    for (uint8_t i = 0; i < s_last_n; i++) if (s_last[i].gpio == gpio) return s_last[i].value;
    return 0;
}

// Caller holds s_mu. The `last` records of the current config, also kept as s_last;
// the caller writes them to NVS after unlocking (a flash write must not stall pin writes).
uint8_t collect_last_locked(LastRec* recs) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_cfg.count; i++) {
        const UserIoPin& p = s_cfg.pins[i];
        if (p.boot != UserIoBoot::LAST || (p.mode != UserIoMode::OUT && p.mode != UserIoMode::PWM)) continue;
        recs[n++] = {p.gpio, 0, static_cast<int16_t>(s_rt[i].value)};
    }
    std::memcpy(s_last, recs, n * sizeof(LastRec));
    s_last_n = n;
    return n;
}

// ── Hardware ──

void pin_off(uint8_t gpio) {   // not driven, no pulls: what an unconfigured pin should be
    gpio_config_t g = {};
    g.pin_bit_mask = 1ULL << gpio;
    g.mode         = GPIO_MODE_DISABLE;
    g.pull_up_en   = GPIO_PULLUP_DISABLE;
    g.pull_down_en = GPIO_PULLDOWN_DISABLE;
    g.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&g);
}

#define FAIL(...) do { std::snprintf(r.err, sizeof(r.err), __VA_ARGS__); return false; } while (0)

bool setup_pwm(const UserIoPin& p, PinRt& r) {
    int t = -1;
    for (int k = 0; k < SOC_LEDC_TIMER_NUM && t < 0; k++) if (s_tmr_users[k] && s_tmr_freq[k] == p.freq) t = k;
    if (t < 0) {
        for (int k = 0; k < SOC_LEDC_TIMER_NUM && t < 0; k++) if (!s_tmr_users[k]) t = k;
        if (t < 0) FAIL("no free PWM timer (%d frequencies max)", SOC_LEDC_TIMER_NUM);
        // As fine as the clock allows, at most 13 bits; the driver picks the clock source.
        int bits = 13;
        while (bits > 1 && (80000000UL >> bits) < p.freq) bits--;
        esp_err_t e = ESP_FAIL;
        for (; bits >= 1 && e != ESP_OK; bits--) {
            ledc_timer_config_t tc = {};
            tc.speed_mode      = LEDC_LOW_SPEED_MODE;
            tc.duty_resolution = static_cast<ledc_timer_bit_t>(bits);
            tc.timer_num       = static_cast<ledc_timer_t>(t);
            tc.freq_hz         = p.freq;
            tc.clk_cfg         = LEDC_AUTO_CLK;
            e = ledc_timer_config(&tc);
            if (e == ESP_OK) s_tmr_bits[t] = static_cast<uint8_t>(bits);
        }
        if (e != ESP_OK) FAIL("PWM %lu Hz is not possible", static_cast<unsigned long>(p.freq));
        s_tmr_freq[t] = p.freq;
    }
    int ch = -1;
    for (int k = 0; k < SOC_LEDC_CHANNEL_NUM && ch < 0; k++) if (!s_ch_used[k]) ch = k;
    if (ch < 0) FAIL("no free PWM channel (%d max)", SOC_LEDC_CHANNEL_NUM);
    ledc_channel_config_t cc = {};
    cc.gpio_num            = p.gpio;
    cc.speed_mode          = LEDC_LOW_SPEED_MODE;
    cc.channel             = static_cast<ledc_channel_t>(ch);
    cc.timer_sel           = static_cast<ledc_timer_t>(t);
    cc.duty                = 0;
    cc.hpoint              = 0;
    cc.flags.output_invert = p.invert;
    if (ledc_channel_config(&cc) != ESP_OK) FAIL("PWM setup failed");
    s_ch_used[ch] = true;
    s_tmr_users[t]++;
    r.ledc_ch   = static_cast<int8_t>(ch);
    r.ledc_tmr  = static_cast<int8_t>(t);
    r.ledc_bits = s_tmr_bits[t];
    return true;
}

bool setup_adc(const UserIoPin& p, PinRt& r) {
    if (adc_oneshot_io_to_channel(p.gpio, &r.adc_unit, &r.adc_chan) != ESP_OK) FAIL("no ADC channel");
    adc_oneshot_unit_handle_t& unit = s_adc[r.adc_unit];
    if (!unit) {
        adc_oneshot_unit_init_cfg_t uc = {};
        uc.unit_id = r.adc_unit;
        if (adc_oneshot_new_unit(&uc, &unit) != ESP_OK) { unit = nullptr; FAIL("ADC unit busy"); }
    }
    adc_oneshot_chan_cfg_t cc = {};
    cc.atten    = kAtten;
    cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit, r.adc_chan, &cc) != ESP_OK) FAIL("ADC setup failed");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cf = {};
    cf.unit_id  = r.adc_unit;
    cf.chan     = r.adc_chan;
    cf.atten    = kAtten;
    cf.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cf, &r.cali) != ESP_OK) {
        r.cali = nullptr;
        std::snprintf(r.err, sizeof(r.err), "no calibration: raw values");
    }
#endif
    r.next_adc_us = 0;
    return true;
}

bool setup_strip(const UserIoPin& p, PinRt& r) {
    led_strip_config_t sc = {};
    sc.strip_gpio_num         = p.gpio;
    sc.max_leds               = p.leds;
    sc.led_model              = LED_MODEL_WS2812;
    sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rc = {};
    rc.clk_src        = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz  = 10 * 1000 * 1000;   // 100 ns ticks: WS2812 timing
    rc.flags.with_dma = false;
    if (led_strip_new_rmt_device(&sc, &rc, &r.strip) != ESP_OK) { r.strip = nullptr; FAIL("no free RMT channel"); }
    led_strip_clear(r.strip);
    return true;
}

// Caller holds s_mu. Sets pin i up in its mode; out/pwm start at `initial`.
bool setup_locked(uint8_t i, int32_t initial) {
    const UserIoPin& p = s_cfg.pins[i];
    PinRt& r = s_rt[i];
    r = PinRt{};
    r.ledc_ch = r.ledc_tmr = -1;
    const auto gpio = static_cast<gpio_num_t>(p.gpio);
    const bool drives = p.mode == UserIoMode::OUT || p.mode == UserIoMode::PWM || p.mode == UserIoMode::STRIP;
    if (drives && !GPIO_IS_VALID_OUTPUT_GPIO(p.gpio)) FAIL("input-only pin");
    switch (p.mode) {
    case UserIoMode::OUT: {
        r.value = initial != 0;
        gpio_set_level(gpio, r.value ^ p.invert);   // latched before the output turns on: no glitch
        gpio_config_t g = {};
        g.pin_bit_mask = 1ULL << p.gpio;
        g.mode         = GPIO_MODE_INPUT_OUTPUT;
        g.intr_type    = GPIO_INTR_DISABLE;
        if (gpio_config(&g) != ESP_OK) FAIL("GPIO setup failed");
        gpio_set_level(gpio, r.value ^ p.invert);
        break;
    }
    case UserIoMode::PWM: {
        if (!setup_pwm(p, r)) return false;
        const double d = initial < 0 ? 0 : initial > 100 ? 100 : initial;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(r.ledc_ch),
                      static_cast<uint32_t>(std::lround(d / 100.0 * (1u << r.ledc_bits))));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(r.ledc_ch));
        r.value = static_cast<int32_t>(d);
        break;
    }
    case UserIoMode::IN: {
        gpio_config_t g = {};
        g.pin_bit_mask = 1ULL << p.gpio;
        g.mode         = GPIO_MODE_INPUT;
        g.pull_up_en   = p.pull == UserIoPull::UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        g.pull_down_en = p.pull == UserIoPull::DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        g.intr_type    = GPIO_INTR_DISABLE;
        if (gpio_config(&g) != ESP_OK) FAIL("GPIO setup failed");
        r.value = gpio_get_level(gpio) ^ p.invert;
        r.have  = true;
        break;
    }
    case UserIoMode::ADC:
        if (!setup_adc(p, r)) return false;
        break;
    case UserIoMode::STRIP:
        if (!setup_strip(p, r)) return false;
        break;
    }
    r.ready = true;
    return true;
}

// Caller holds s_mu. Undoes pin i and leaves it undriven.
void release_locked(uint8_t i) {
    const UserIoPin& p = s_cfg.pins[i];
    PinRt& r = s_rt[i];
    if (r.ledc_ch >= 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(r.ledc_ch), 0);
        s_ch_used[r.ledc_ch] = false;
        if (r.ledc_tmr >= 0 && s_tmr_users[r.ledc_tmr]) s_tmr_users[r.ledc_tmr]--;
    }
    if (r.strip) {
        led_strip_clear(r.strip);
        led_strip_del(r.strip);
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (r.cali) adc_cali_delete_scheme_curve_fitting(r.cali);
#endif
    pin_off(p.gpio);
    r = PinRt{};
    r.ledc_ch = r.ledc_tmr = -1;
}

#undef FAIL

bool adc_read_locked(uint8_t i, int32_t* value, int32_t* raw) {
    PinRt& r = s_rt[i];
    if (!r.ready || !s_adc[r.adc_unit]) return false;
    int v = 0;
    if (adc_oneshot_read(s_adc[r.adc_unit], r.adc_chan, &v) != ESP_OK) return false;
    *raw = v;
    int mv = v;
    if (r.cali && adc_cali_raw_to_voltage(r.cali, v, &mv) != ESP_OK) mv = v;
    *value = mv;
    return true;
}

void fill_pub(Pub* pub, const UserIoPin& p, int32_t v) {
    if (!pub) return;
    std::snprintf(pub->key, sizeof(pub->key), "%s", p.name);
    pub->vt = (p.mode == UserIoMode::OUT || p.mode == UserIoMode::IN) ? VAL_BOOL : VAL_INT;
    pub->v  = v;
}

// Caller holds s_mu. out: v != 0 = on; pwm: duty %, clamped.
bool write_locked(uint8_t i, double v, Pub* pub) {
    const UserIoPin& p = s_cfg.pins[i];
    PinRt& r = s_rt[i];
    if (!r.ready || v != v) return false;
    if (p.mode == UserIoMode::OUT) {
        const int32_t lvl = v != 0.0;
        if (gpio_set_level(static_cast<gpio_num_t>(p.gpio), lvl ^ p.invert) != ESP_OK) return false;
        r.value = lvl;
    } else if (p.mode == UserIoMode::PWM) {
        const double d = v < 0 ? 0 : v > 100 ? 100 : v;
        const auto ch = static_cast<ledc_channel_t>(r.ledc_ch);
        if (ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, static_cast<uint32_t>(std::lround(d / 100.0 * (1u << r.ledc_bits)))) != ESP_OK ||
            ledc_update_duty(LEDC_LOW_SPEED_MODE, ch) != ESP_OK)
            return false;
        r.value = static_cast<int32_t>(std::lround(d));
    } else {
        return false;
    }
    if (p.boot == UserIoBoot::LAST) s_last_dirty_us = esp_timer_get_time();
    fill_pub(pub, p, r.value);
    return true;
}

void publish(const Pub* pubs, size_t n) {
    for (size_t k = 0; k < n; k++) {
        ZclAttribute a{};
        zcl_attr_set_int(&a, pubs[k].key, pubs[k].v, pubs[k].vt);
        device_shadow_process_ieee(kHubIoIeee, &a, 1);
    }
}

// The pin `ref` names, in mode `want`; else -1 with the reason in err. Caller holds s_mu.
int find_locked(const char* ref, UserIoMode want, char* err, size_t cap) {
    const int i = user_io_cfg_find(s_cfg, ref);
    if (i < 0) { std::snprintf(err, cap, "unknown pin '%s'", ref ? ref : ""); return -1; }
    if (s_cfg.pins[i].mode != want) {
        std::snprintf(err, cap, "pin '%s' is %s, not %s", ref, user_io_mode_name(s_cfg.pins[i].mode), user_io_mode_name(want));
        return -1;
    }
    if (!s_rt[i].ready) { std::snprintf(err, cap, "pin '%s': %s", ref, s_rt[i].err); return -1; }
    return i;
}

// ── Hooks: rules, device_cmd, exposes ──

bool rules_check(SimpleRulesIoOp op, const char* pin, char* err, size_t cap) {
    const UserIoMode want = op == SimpleRulesIoOp::PWM ? UserIoMode::PWM : UserIoMode::OUT;
    Lock l;
    const int i = user_io_cfg_find(s_cfg, pin);
    if (i >= 0 && s_cfg.pins[i].mode == want) return true;
    char names[100] = "";
    size_t n = 0;
    for (uint8_t k = 0; k < s_cfg.count && n < sizeof(names); k++) {
        if (s_cfg.pins[k].mode != want) continue;
        n += static_cast<size_t>(std::snprintf(names + n, sizeof(names) - n, "%s%s", n ? ", " : "", s_cfg.pins[k].name));
    }
    const char* kind = want == UserIoMode::PWM ? "pwm pins" : "outputs";
    if (i < 0) std::snprintf(err, cap, "unknown pin '%s' (%s: %s)", pin, kind, n ? names : "none configured");
    else       std::snprintf(err, cap, "pin '%s' is %s (%s: %s)", pin, user_io_mode_name(s_cfg.pins[i].mode), kind, n ? names : "none configured");
    return false;
}

bool rules_run(SimpleRulesIoOp op, const char* pin, const char* value) {
    char err[64] = "";
    bool ok = false;
    if (!value) value = "";
    if (op == SimpleRulesIoOp::TOGGLE) {
        ok = user_io_toggle(pin, err, sizeof(err)) >= 0;
    } else if (op == SimpleRulesIoOp::SET) {
        bool on = false;
        if (!user_io_parse_level(value, &on)) std::snprintf(err, sizeof(err), "'%s' is not on or off", value);
        else ok = user_io_write(pin, UserIoMode::OUT, on ? 1.0 : 0.0, err, sizeof(err));
    } else {
        char* end = nullptr;
        const double d = std::strtod(value, &end);
        if (end == value || *end != '\0') std::snprintf(err, sizeof(err), "'%s' is not a duty", value);
        else ok = user_io_write(pin, UserIoMode::PWM, d, err, sizeof(err));
    }
    if (!ok) ESP_LOGW(TAG, "rule: %s", err);
    return ok;
}

DevCmdResult local_cmd(uint64_t ieee, const char* key, const DevCmdValue* v) {
    if (ieee != kHubIoIeee) return DEVCMD_NOT_FOUND;
    UserIoMode mode;
    {
        Lock l;
        int i = -1;
        for (uint8_t k = 0; k < s_cfg.count && i < 0; k++) if (std::strcmp(s_cfg.pins[k].name, key) == 0) i = k;
        if (i < 0) return DEVCMD_NO_CONVERTER;
        mode = s_cfg.pins[i].mode;
    }
    if (mode != UserIoMode::OUT && mode != UserIoMode::PWM) return DEVCMD_NO_CONVERTER;   // read-only
    double x = 0;
    switch (v->kind) {
    case DEVCMD_BOOL:  x = v->b ? (mode == UserIoMode::PWM ? 100.0 : 1.0) : 0.0; break;
    case DEVCMD_INT:   x = static_cast<double>(v->i); break;
    case DEVCMD_FLOAT: x = v->f; break;
    case DEVCMD_STR: {   // "on"/"off" for both; a duty for pwm
        bool on = false;
        char* end = nullptr;
        if (!v->s) return DEVCMD_BAD_VALUE;
        if (mode == UserIoMode::PWM) x = std::strtod(v->s, &end);
        if (mode == UserIoMode::PWM && end != v->s && *end == '\0') break;
        if (!user_io_parse_level(v->s, &on)) return DEVCMD_BAD_VALUE;
        x = on ? (mode == UserIoMode::PWM ? 100.0 : 1.0) : 0.0;
        break;
    }
    }
    char err[64];
    if (user_io_write(key, mode, x, err, sizeof(err))) return DEVCMD_OK;
    ESP_LOGW(TAG, "set %s: %s", key, err);
    return DEVCMD_NO_CONVERTER;
}

size_t local_exposes(uint64_t ieee, char* buf, size_t cap) {
    if (ieee != kHubIoIeee) return 0;
    const bool mv = user_io_adc_in_mv();
    Lock l;
    size_t n = 0;
    bool ok = true;
    auto put = [&](const char* s) {
        const size_t m = std::strlen(s);
        if (!ok || n + m + 1 > cap) { ok = false; return; }
        std::memcpy(buf + n, s, m + 1);
        n += m;
    };
    put("[");
    bool first = true;
    for (uint8_t i = 0; i < s_cfg.count; i++) {
        const UserIoPin& p = s_cfg.pins[i];
        char one[160];
        switch (p.mode) {
        case UserIoMode::OUT:
            std::snprintf(one, sizeof(one), "{\"name\":\"%s\",\"type\":\"binary\",\"access\":7}", p.name);
            break;
        case UserIoMode::PWM:
            std::snprintf(one, sizeof(one), "{\"name\":\"%s\",\"type\":\"numeric\",\"access\":7,\"unit\":\"%%\","
                          "\"value_min\":0,\"value_max\":100,\"value_step\":1}", p.name);
            break;
        case UserIoMode::IN:
            std::snprintf(one, sizeof(one), "{\"name\":\"%s\",\"type\":\"binary\",\"access\":1}", p.name);
            break;
        case UserIoMode::ADC:
            if (!p.interval) continue;   // Lua only: never published
            std::snprintf(one, sizeof(one), "{\"name\":\"%s\",\"type\":\"numeric\",\"access\":1%s}", p.name,
                          mv ? ",\"unit\":\"mV\"" : "");
            break;
        case UserIoMode::STRIP:
            continue;   // Lua only
        }
        if (!first) put(",");
        put(one);
        first = false;
    }
    put("]");
    return ok ? n : 0;
}

// ── The Hub I/O device in the pool ──

void reconcile_device(bool announce) {
    bool want;
    {
        Lock l;
        want = s_cfg.count > 0;
    }
    // Static: called from app_main at boot (tight stack) and from the one /api/io PUT at a time.
    static ZapDevice snap, before;
    snap = ZapDevice{};
    bool save = false, removed = false;
    zigbee_pool_lock();
    ZapDevice* d = pool_find_by_ieee(kHubIoIeee);
    if (want) {
        bool added = false;
        if (!d && (d = pool_add()) != nullptr) {
            d->ieee_addr = kHubIoIeee;
            std::snprintf(d->friendly_name, sizeof(d->friendly_name), "Hub I/O");
            added = true;
        }
        if (d) {
            // What the hub expects of it, also over an older or soft-removed record.
            before = *d;
            d->protocol        = PROTO_LOCAL;
            d->nwk_addr        = 0xFFFE;   // "no network address"
            d->device_type     = 1;        // mains powered, always there
            d->power_source    = 1;
            d->flags          &= static_cast<uint8_t>(~(ZAP_DEV_REMOVED | ZAP_DEV_FIX_BINDS | ZAP_DEV_FIX_CIE));
            d->interview_state = static_cast<uint8_t>(InterviewState::IDENTITY_READY);
            d->support_state   = static_cast<uint8_t>(SupportState::MATCHED);
            d->configure_state = static_cast<uint8_t>(ConfigureState::DONE);
            std::snprintf(d->model_id, sizeof(d->model_id), "ZHAC-IO");
            std::snprintf(d->manufacturer_name, sizeof(d->manufacturer_name), "ZHAC");
            const time_t now = time(nullptr);
            if (now > 1577836800) d->last_seen = static_cast<uint32_t>(now);
            save = added || std::memcmp(before.model_id, d->model_id, sizeof(d->model_id)) != 0 ||
                   before.protocol != d->protocol || before.flags != d->flags ||
                   before.support_state != d->support_state || before.interview_state != d->interview_state ||
                   before.configure_state != d->configure_state;
            snap = *d;
        }
    } else if (d && announce) {
        // Only a save that empties the config removes it. At boot an empty config may be
        // a stored one this build refused: the record (and its name) stays for the owner.
        pool_remove(static_cast<uint16_t>(d - pool_all()));
        removed = true;
    }
    zigbee_pool_unlock();
    if (want && !snap.ieee_addr) ESP_LOGE(TAG, "device pool full: the Hub I/O device is not shown");
    if (save && !zap_store_save_device(&snap)) ESP_LOGW(TAG, "Hub I/O device not saved");
    if (removed) {
        zap_store_delete_device(kHubIoIeee);
        device_shadow_remove(kHubIoIeee);
    }
    if (announce && (snap.ieee_addr || removed)) {
        Event ev{};
        ev.type = snap.ieee_addr ? EventType::DEVICE_JOIN : EventType::DEVICE_LEAVE;
        std::memcpy(ev.data, &kHubIoIeee, sizeof(kHubIoIeee));
        event_bus_publish(ev);
    }
}

// Caller holds s_mu. The value of every pin that shows on the device.
size_t current_pubs_locked(Pub* pubs) {
    size_t n = 0;
    for (uint8_t i = 0; i < s_cfg.count; i++) {
        const UserIoPin& p = s_cfg.pins[i];
        const PinRt& r = s_rt[i];
        if (!r.ready || p.mode == UserIoMode::STRIP) continue;
        if (p.mode == UserIoMode::ADC && (!p.interval || !r.have)) continue;
        fill_pub(&pubs[n++], p, r.value);
    }
    return n;
}

void io_task(void*) {
    int64_t next_seen_us = 0;
    bool stack_logged = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        Pub pubs[kUserIoMaxPins];
        LastRec recs[kUserIoMaxPins];
        int n_last = -1;   // >= 0: write these `last` records
        size_t n = 0;
        const int64_t now = esp_timer_get_time();
        {
            Lock l;
            for (uint8_t i = 0; i < s_cfg.count; i++) {
                const UserIoPin& p = s_cfg.pins[i];
                PinRt& r = s_rt[i];
                if (!r.ready) continue;
                if (p.mode == UserIoMode::IN) {
                    const int32_t lvl = gpio_get_level(static_cast<gpio_num_t>(p.gpio)) ^ p.invert;
                    if (lvl == r.value) { r.settle = 0; continue; }
                    if (++r.settle < 2) continue;
                    r.settle = 0;
                    r.value  = lvl;
                    fill_pub(&pubs[n++], p, lvl);
                } else if (p.mode == UserIoMode::ADC && p.interval && now >= r.next_adc_us) {
                    r.next_adc_us = now + static_cast<int64_t>(p.interval) * 1000000;
                    int32_t v = 0, raw = 0;
                    if (!adc_read_locked(i, &v, &raw)) continue;
                    if (r.have && std::abs(v - r.value) < p.delta) continue;
                    r.value = v;
                    r.have  = true;
                    fill_pub(&pubs[n++], p, v);
                }
            }
            if (s_last_dirty_us && now - s_last_dirty_us >= kLastSaveDelayUs) {
                s_last_dirty_us = 0;
                n_last = collect_last_locked(recs);
            }
        }
        // The only writer of `last`, so an older set never lands after a newer one.
        if (n_last >= 0) {
            if (!nvs_put("last", recs, static_cast<size_t>(n_last) * sizeof(LastRec)))
                ESP_LOGW(TAG, "could not save the pin levels");
            if (!stack_logged) {   // for the hardware check: the NVS path is this task's deepest
                stack_logged = true;
                ESP_LOGI(TAG, "poll task: %u B of stack never used", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
        }
        publish(pubs, n);
        if (now >= next_seen_us) {   // the device is always there: keep "last seen" current
            next_seen_us = now + 60LL * 1000000;
            const time_t t = time(nullptr);
            zigbee_pool_lock();
            if (ZapDevice* d = pool_find_by_ieee(kHubIoIeee); d && t > 1577836800) d->last_seen = static_cast<uint32_t>(t);
            zigbee_pool_unlock();
        }
    }
}

// The poll task exists only once a pin does: most hubs have none, and it costs a 6 KB internal stack and a wake
// every 20 ms (research 2026-11-18 A8). Pins removed later leave it running until the next boot. Lock held.
void start_task_locked() {
    if (s_task || !s_cfg.count) return;
    // 6 KB like the other tasks that commit NVS (zhac::stack::kZapFlush: ~3 KB used by the commit path).
    // Internal RAM (xTaskCreate): it writes flash, so its stack cannot sit in PSRAM.
    if (xTaskCreate(io_task, "user_io", 6144, nullptr, 2, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "poll task not started: inputs and ADC are not read");
    }
}

}  // namespace

// ── Public ──

const UserIoCaps& user_io_caps() { return s_caps; }

bool user_io_adc_in_mv() {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    return true;
#else
    return false;
#endif
}

void user_io_early_init() {
    s_mu = xSemaphoreCreateMutex();
    build_caps();
    s_last_n = static_cast<uint8_t>(nvs_get("last", s_last, sizeof(s_last)) / sizeof(LastRec));

    char* json = static_cast<char*>(std::malloc(kCfgJsonMax));
    const size_t len = json ? nvs_get("cfg", json, kCfgJsonMax) : 0;
    char err[96] = "";
    // No hooks are registered yet, so nothing else reads s_cfg: parse straight into it
    // (a UserIoConfig is ~600 B, too much for app_main's stack).
    if (len && !user_io_cfg_from_json(json, len, s_caps, &s_cfg, err, sizeof(err), true)) {
        ESP_LOGE(TAG, "stored pin config unreadable (%s): no pins this boot", err);
        s_cfg = UserIoConfig{};
    } else if (err[0]) {
        // The NVS copy stays until the next save: a build that allows the pin brings the row back.
        ESP_LOGW(TAG, "stored pin config: %s; that row is off on this build, the others are on", err);
    }
    std::free(json);
    {
        Lock l;
        for (uint8_t i = 0; i < s_cfg.count; i++) {
            const UserIoPin& p = s_cfg.pins[i];
            const int32_t full = p.mode == UserIoMode::PWM ? 100 : 1;
            const int32_t initial = p.boot == UserIoBoot::ON ? full : p.boot == UserIoBoot::LAST ? last_value(p.gpio) : 0;
            if (!setup_locked(i, initial)) ESP_LOGW(TAG, "GPIO %u (%s): %s", p.gpio, p.name, s_rt[i].err);
        }
    }
    device_cmd_set_local_handler(local_cmd);
    zhac_adapter_set_local_exposes_fn(local_exposes);
    simple_rules_set_io_hooks(rules_check, rules_run);
    ESP_LOGI(TAG, "%u pin(s) configured, %u allowed (%s)", s_cfg.count, s_caps.n_allowed, CONFIG_ZHAC_IO_PINS);
}

void user_io_attach_device() { reconcile_device(false); }

void user_io_start() {
    // Values restored for pins that are not configured now (a row dropped at boot) leave the device.
    if (auto* attrs = static_cast<ShadowAttr*>(std::malloc(32 * sizeof(ShadowAttr)))) {
        const uint8_t na = device_shadow_get_attrs(kHubIoIeee, attrs, 32);
        for (uint8_t k = 0; k < na; k++) {
            bool configured;
            {
                Lock l;
                configured = user_io_cfg_find(s_cfg, attrs[k].key) >= 0;
            }
            if (!configured) device_shadow_remove_attr(kHubIoIeee, attrs[k].key);
        }
        std::free(attrs);
    }
    Pub pubs[kUserIoMaxPins];
    size_t n;
    {
        Lock l;
        n = current_pubs_locked(pubs);
    }
    publish(pubs, n);
    Lock l;
    start_task_locked();
}

bool user_io_apply(const UserIoConfig& next, char* err, size_t cap) {
    // The previous list, kept while the new one is set up: ~2 KB on the heap, not held as statics for good
    // (research 2026-11-18 A8). Taken before the save, so a failure leaves the stored config as it was.
    struct Old { UserIoConfig cfg; PinRt rt[kUserIoMaxPins]; };
    const std::unique_ptr<Old> prev(new (std::nothrow) Old);
    if (!prev) { std::snprintf(err, cap, "out of memory"); return false; }
    char* json = static_cast<char*>(std::malloc(kCfgJsonMax));
    const size_t len = json ? cfg_json(next, json, kCfgJsonMax) : 0;
    const bool saved = len && nvs_put("cfg", json, len);
    std::free(json);
    if (!saved) { std::snprintf(err, cap, "could not save the pin config"); return false; }

    Pub pubs[kUserIoMaxPins];
    size_t n;
    char gone[kUserIoMaxPins][kUserIoNameMax];
    uint8_t n_gone = 0;
    {
        Lock l;
        UserIoConfig& old = prev->cfg;
        PinRt* const old_rt = prev->rt;
        old = s_cfg;
        std::memcpy(old_rt, s_rt, sizeof(s_rt));
        // A pin whose hardware setup is unchanged keeps running untouched (a relay does not
        // drop, a strip keeps its colours); only the others are released and set up again.
        int8_t keep_from[kUserIoMaxPins];
        bool kept_old[kUserIoMaxPins] = {};
        for (uint8_t i = 0; i < next.count; i++) {
            keep_from[i] = -1;
            for (uint8_t k = 0; k < old.count && keep_from[i] < 0; k++) {
                if (kept_old[k] || !old_rt[k].ready || !user_io_pin_same_hw(old.pins[k], next.pins[i])) continue;
                keep_from[i] = static_cast<int8_t>(k);
                kept_old[k] = true;
            }
        }
        for (uint8_t k = 0; k < old.count; k++) if (!kept_old[k]) release_locked(k);   // s_cfg is still the old list
        s_cfg = next;
        for (uint8_t i = 0; i < kUserIoMaxPins; i++) { s_rt[i] = PinRt{}; s_rt[i].ledc_ch = s_rt[i].ledc_tmr = -1; }
        for (uint8_t i = 0; i < s_cfg.count; i++) {
            const UserIoPin& p = s_cfg.pins[i];
            if (keep_from[i] >= 0) { s_rt[i] = old_rt[keep_from[i]]; continue; }
            // Same pin and mode with other settings (a new frequency) keeps its level; a new pin starts as `boot` says.
            int32_t initial = p.boot == UserIoBoot::ON ? (p.mode == UserIoMode::PWM ? 100 : 1)
                            : p.boot == UserIoBoot::LAST ? last_value(p.gpio) : 0;
            for (uint8_t k = 0; k < old.count; k++)
                if (old.pins[k].gpio == p.gpio && old.pins[k].mode == p.mode) initial = old_rt[k].value;
            if (!setup_locked(i, initial)) ESP_LOGW(TAG, "GPIO %u (%s): %s", p.gpio, p.name, s_rt[i].err);
        }
        for (uint8_t k = 0; k < old.count; k++) {
            bool kept = false;
            for (uint8_t i = 0; i < s_cfg.count; i++) kept |= std::strcmp(old.pins[k].name, s_cfg.pins[i].name) == 0;
            if (!kept) std::memcpy(gone[n_gone++], old.pins[k].name, kUserIoNameMax);
        }
        s_last_dirty_us = esp_timer_get_time();   // the poll task rewrites `last` for the new pin list
        n = current_pubs_locked(pubs);
    }
    for (uint8_t k = 0; k < n_gone; k++) device_shadow_remove_attr(kHubIoIeee, gone[k]);
    reconcile_device(true);   // hub page, cloud and HA re-read the device: its exposes changed
    publish(pubs, n);
    {
        Lock l;
        start_task_locked();
    }
    simple_rules_reload();    // gpio verbs re-check their pins
    ESP_LOGI(TAG, "pin config applied: %u pin(s)", next.count);
    return true;
}

void user_io_snapshot(UserIoState* out) {
    Lock l;
    out->cfg = s_cfg;
    for (uint8_t i = 0; i < s_cfg.count; i++) {
        out->value[i] = s_rt[i].value;
        std::memcpy(out->err[i], s_rt[i].err, sizeof(out->err[i]));
    }
}

bool user_io_write(const char* ref, UserIoMode want, double v, char* err, size_t cap) {
    Pub pub;
    {
        Lock l;
        const int i = find_locked(ref, want, err, cap);
        if (i < 0) return false;
        if (!write_locked(static_cast<uint8_t>(i), v, &pub)) { std::snprintf(err, cap, "pin '%s': write failed", ref); return false; }
    }
    publish(&pub, 1);
    return true;
}

int user_io_toggle(const char* ref, char* err, size_t cap) {
    Pub pub;
    int32_t lvl;
    {
        Lock l;
        const int i = find_locked(ref, UserIoMode::OUT, err, cap);
        if (i < 0) return -1;
        if (!write_locked(static_cast<uint8_t>(i), s_rt[i].value ? 0.0 : 1.0, &pub)) {
            std::snprintf(err, cap, "pin '%s': write failed", ref);
            return -1;
        }
        lvl = s_rt[i].value;
    }
    publish(&pub, 1);
    return lvl;
}

bool user_io_read(const char* ref, int32_t* value, int32_t* raw, char* err, size_t cap) {
    Lock l;
    const int i = user_io_cfg_find(s_cfg, ref);
    if (i < 0) { std::snprintf(err, cap, "unknown pin '%s'", ref ? ref : ""); return false; }
    const UserIoPin& p = s_cfg.pins[i];
    const PinRt& r = s_rt[i];
    if (!r.ready) { std::snprintf(err, cap, "pin '%s': %s", ref, r.err); return false; }
    switch (p.mode) {
    case UserIoMode::OUT:
        *value = *raw = r.value;
        return true;
    case UserIoMode::IN:
        *value = *raw = gpio_get_level(static_cast<gpio_num_t>(p.gpio)) ^ p.invert;
        return true;
    case UserIoMode::ADC:
        if (adc_read_locked(static_cast<uint8_t>(i), value, raw)) return true;
        std::snprintf(err, cap, "pin '%s': ADC read failed", ref);
        return false;
    default:
        std::snprintf(err, cap, "pin '%s' is %s: nothing to read", ref, user_io_mode_name(p.mode));
        return false;
    }
}

bool user_io_strip(const char* ref, const uint32_t* rgb, size_t n, char* err, size_t cap) {
    Lock l;
    const int i = find_locked(ref, UserIoMode::STRIP, err, cap);
    if (i < 0) return false;
    led_strip_handle_t strip = s_rt[i].strip;
    const size_t leds = s_cfg.pins[i].leds;
    for (size_t k = 0; k < n && k < leds; k++) {
        if (led_strip_set_pixel(strip, k, (rgb[k] >> 16) & 0xFF, (rgb[k] >> 8) & 0xFF, rgb[k] & 0xFF) != ESP_OK) {
            std::snprintf(err, cap, "pin '%s': LED %u not set", ref, static_cast<unsigned>(k));
            return false;
        }
    }
    if (led_strip_refresh(strip) != ESP_OK) { std::snprintf(err, cap, "pin '%s': strip refresh failed", ref); return false; }
    return true;
}
