// SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// Adapted for ZHAC from esp-bsp components/es8311/es8311.c (commit f259a6a), by way of HomeTiles'
// Apache-2.0 port (src/audio/codecs/es8311/es8311_codec.cpp).
//   Taken: es8311_init()'s register sequence with es8311_clock_config() (MCLK from the MCLK pin, no
//   inversion), es8311_fmt_config() (slave, 16-bit) and es8311_sample_frequency_config() for one
//   coeff_div[] row, verbatim; the DAC volume (0x32) and mute (0x31) registers.
//   Changed: registers go over i2c_master; HomeTiles' chip-ID check is in the probe; one rate only; the
//   microphone, fade, the other rates and the register dump are left out.
#include "es8311.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG        = "es8311";
constexpr uint16_t    kAddr      = 0x18;   // CE low, as on both boards
constexpr int         kTimeoutMs = 50;

// The esp-bsp coeff_div[] row for MCLK 4.096 MHz = 256 x 16 kHz.
struct CoeffDiv {
    uint32_t mclk, rate;
    uint8_t  pre_div, pre_multi, adc_div, dac_div, fs_mode, lrck_h, lrck_l, bclk_div, adc_osr, dac_osr;
};
constexpr CoeffDiv kRow = {4096000, 16000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10};

esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v) {
    const uint8_t b[2] = {reg, v};
    return i2c_master_transmit(d, b, sizeof(b), kTimeoutMs);
}

esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t* v) {
    return i2c_master_transmit_receive(d, &reg, 1, v, 1, kTimeoutMs);
}

// Read, keep the bits in `keep`, add `set`, write back.
esp_err_t rmw(i2c_master_dev_handle_t d, uint8_t reg, uint8_t keep, uint8_t set) {
    uint8_t v = 0;
    const esp_err_t e = rd(d, reg, &v);
    return e != ESP_OK ? e : wr(d, reg, static_cast<uint8_t>((v & keep) | set));
}

#define TRY(x)                                  \
    do {                                        \
        const esp_err_t e_ = (x);               \
        if (e_ != ESP_OK) return e_;            \
    } while (0)

esp_err_t dac_start(i2c_master_dev_handle_t d) {
    // es8311_init(): reset to the defaults, then the power-on command.
    TRY(wr(d, 0x00, 0x1F));
    vTaskDelay(pdMS_TO_TICKS(20));
    TRY(wr(d, 0x00, 0x00));
    TRY(wr(d, 0x00, 0x80));
    // es8311_clock_config(): MCLK from the MCLK pin, not inverted, every clock on; SCLK not inverted.
    TRY(wr(d, 0x01, 0x3F));
    TRY(rmw(d, 0x06, 0xDF, 0));
    // es8311_sample_frequency_config() with kRow.
    TRY(rmw(d, 0x02, 0x07, ((kRow.pre_div - 1) << 5) | (kRow.pre_multi << 3)));
    TRY(wr(d, 0x03, (kRow.fs_mode << 6) | kRow.adc_osr));
    TRY(wr(d, 0x04, kRow.dac_osr));
    TRY(wr(d, 0x05, ((kRow.adc_div - 1) << 4) | (kRow.dac_div - 1)));
    TRY(rmw(d, 0x06, 0xE0, kRow.bclk_div < 19 ? kRow.bclk_div - 1 : kRow.bclk_div));
    TRY(rmw(d, 0x07, 0xC0, kRow.lrck_h));
    TRY(wr(d, 0x08, kRow.lrck_l));
    // es8311_fmt_config(): slave serial port; 16-bit in (to the DAC) and out (from the ADC).
    TRY(rmw(d, 0x00, 0xBF, 0));
    TRY(wr(d, 0x09, 3 << 2));
    TRY(wr(d, 0x0A, 3 << 2));
    // The rest of es8311_init(), as esp-bsp has it. Its two ADC lines stay: the sequence is the proven one,
    // and nothing reads the ADC (no I2S RX channel).
    TRY(wr(d, 0x0D, 0x01));   // power up the analog circuits
    TRY(wr(d, 0x0E, 0x02));   // analog PGA, ADC modulator
    TRY(wr(d, 0x12, 0x00));   // power up the DAC
    TRY(wr(d, 0x13, 0x10));   // output to the HP driver
    TRY(wr(d, 0x1C, 0x6A));   // ADC equalizer bypass, DC offset cancel
    TRY(wr(d, 0x37, 0x08));   // bypass the DAC equalizer
    // es8311_voice_volume_set() at 0 dB (0xBF; 0.5 dB steps), es8311_voice_mute(false): the loudness is the
    // synthesizer's alone (sound_synth.cpp), held to the board's maximum by the player.
    TRY(wr(d, 0x32, 0xBF));
    return rmw(d, 0x31, 0x9F, 0);
}

#undef TRY

}  // namespace

bool es8311_probe(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t* dev) {
    if (i2c_master_probe(bus, kAddr, kTimeoutMs) != ESP_OK) return false;   // no ACK (logs nothing)
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = kAddr;
    cfg.scl_speed_hz    = 100000;
    i2c_master_dev_handle_t h = nullptr;
    if (i2c_master_bus_add_device(bus, &cfg, &h) != ESP_OK) return false;
    uint8_t id1 = 0, id2 = 0;
    if (rd(h, 0xFD, &id1) == ESP_OK && rd(h, 0xFE, &id2) == ESP_OK && id1 == 0x83 && id2 == 0x11) {
        *dev = h;
        return true;
    }
    ESP_LOGW(TAG, "0x%02X answers, but its chip ID reads %02X%02X, not 8311", kAddr, id1, id2);
    i2c_master_bus_rm_device(h);
    return false;
}

bool es8311_dac_start(i2c_master_dev_handle_t dev, esp_err_t* err) {
    const esp_err_t e = dac_start(dev);
    if (err) *err = e;
    return e == ESP_OK;
}
