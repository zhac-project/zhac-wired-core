// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// status_led.cpp — see status_led.h.
#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_ZHAC_STATUS_LED_GPIO >= 0

#include <cstdint>

#include "device_cmd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "led_strip.h"

static const char* TAG = "status_led";

namespace {

constexpr uint8_t  kLevel      = 40;    // out of 255: a dev-board LED is bright
constexpr uint32_t kTickMs     = 100;
constexpr uint32_t kFlashMs    = 120;   // one blue flash per traffic burst
constexpr uint32_t kBlinkTicks = 5;     // green: 500 ms on, 500 ms off

led_strip_handle_t s_strip = nullptr;
esp_timer_handle_t s_tick  = nullptr;
volatile uint32_t  s_flash_until_ms = 0;   // written on the pump task, read on the timer task
uint32_t           s_ticks = 0;
uint8_t            s_cur[3] = {0, 0, 0};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Only touch the strip when the colour changes: one RMT frame per change.
void show(uint8_t r, uint8_t g, uint8_t b) {
    if (r == s_cur[0] && g == s_cur[1] && b == s_cur[2]) return;
    s_cur[0] = r; s_cur[1] = g; s_cur[2] = b;
    if (!r && !g && !b) { led_strip_clear(s_strip); return; }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void on_tick(void*) {
    s_ticks++;
    if (static_cast<int32_t>(s_flash_until_ms - now_ms()) > 0) { show(0, 0, kLevel); return; }
    bool open = false;
    int  remaining = 0;
    device_cmd_permit_join_status(&open, &remaining);
    if (open) { show(0, ((s_ticks / kBlinkTicks) & 1) ? kLevel : 0, 0); return; }
    show(0, 0, 0);
}

void on_traffic(const Event&) { s_flash_until_ms = now_ms() + kFlashMs; }

}  // namespace

void status_led_start(void) {
    led_strip_config_t sc = {
        .strip_gpio_num         = CONFIG_ZHAC_STATUS_LED_GPIO,
        .max_leds               = 1,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags                  = { .invert_out = false },
    };
    led_strip_rmt_config_t rc = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 100 ns ticks: WS2812 timing
        .mem_block_symbols = 0,   // driver default
        .flags         = { .with_dma = false },
    };
    const esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no status LED on GPIO %d (%s)", CONFIG_ZHAC_STATUS_LED_GPIO, esp_err_to_name(e));
        s_strip = nullptr;
        return;
    }
    led_strip_clear(s_strip);
    event_bus_subscribe(EventType::ZCL_ATTR,    on_traffic);
    event_bus_subscribe(EventType::ZCL_RAW,     on_traffic);
    event_bus_subscribe(EventType::DEVICE_JOIN, on_traffic);
    const esp_timer_create_args_t args = { .callback = on_tick, .arg = nullptr,
                                           .dispatch_method = ESP_TIMER_TASK, .name = "status_led",
                                           .skip_unhandled_events = true };
    if (esp_timer_create(&args, &s_tick) == ESP_OK) esp_timer_start_periodic(s_tick, kTickMs * 1000);
    ESP_LOGI(TAG, "status LED on GPIO %d: green blink = join open, blue flash = Zigbee traffic",
             CONFIG_ZHAC_STATUS_LED_GPIO);
}

#else
void status_led_start(void) {}
#endif
