// SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// Adapted for ZHAC from esp-bsp components/es8311/es8311.c: the ES8311 codec's playback (DAC) side only,
// at one rate. The codec is an I2S slave taking 16-bit samples at 16 kHz, with MCLK = 256 x fs on its
// MCLK pin. Registers go over i2c_master. es8311.cpp says what was taken and what changed.
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// True when a device answers at 0x18 and its chip-ID registers read 0xFD = 0x83, 0xFE = 0x11: *dev is then
// the codec on `bus`. An ACK alone is not enough. False leaves nothing added to the bus.
bool es8311_probe(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t* dev);

// Resets the codec and starts playback: the MCLK 4.096 MHz (256 x 16 kHz) clock row, I2S slave 16-bit,
// DAC powered at 0 dB, unmuted. Called with MCLK running, as in esp-bsp's examples. False on an I2C error,
// which goes to *err when given.
bool es8311_dac_start(i2c_master_dev_handle_t dev, esp_err_t* err = nullptr);
