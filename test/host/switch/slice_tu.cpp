// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL system_apply_settings, cut out of main/api_system.cpp by run.sh, with its neighbours stubbed.
#include "stubs.h"
#include "sys_state.h"
#include "log_cloud.h"   // logcloud::*, log_cloud_on_setting()
#include "ws_bridge.h"   // the real declaration of ws_push_hub_caps()
static const char* TAG = "api_system";
#include "system_apply_settings.inc"
