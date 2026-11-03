# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# tg_gw_stub -- the Telegram calls on this firmware.
#
# tg_gw is the dual-chip Telegram gateway: its P4 half forwards over HAP to an
# S3 that this firmware does not have. Since review 2026-10-28 it supplies no
# sources to a single-chip build (this project sets ZHAC_MQTT_GW_LOCAL_CLIENT),
# so these stubs answer on every target here: Lua's telegram_* return false and
# the log says the gateway is not on this SKU. Before, the P4 build linked
# tg_gw_p4.cpp, whose HAP send to nowhere failed with a misleading HAP error.
# The cloud's Telegram bot is the way to reach Telegram from a wired hub.
idf_component_register(SRCS "tg_gw_stub.cpp" REQUIRES tg_gw)
