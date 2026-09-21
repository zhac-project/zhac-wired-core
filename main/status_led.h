// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// status_led — one addressable RGB LED (WS2812) as the hub's face:
//   green blink  while the join window is open
//   blue flash   on Zigbee traffic (a report, a raw frame, a join)
//   off          otherwise
// GPIO from CONFIG_ZHAC_STATUS_LED_GPIO; -1 builds it out entirely.
#pragma once

// After event_bus_init(): subscribes to the traffic events and starts a
// 100 ms timer that paints the LED. Safe to call on a board without one.
void status_led_start(void);
