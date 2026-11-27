// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The REAL system_apply_settings, cut out of main/api_system.cpp by run.sh, with its neighbours stubbed, and the
// REAL relay hooks that read the settings (one line each in main/ws_bridge.cpp, cut out by run.sh).
#include "stubs.h"
#include "sys_state.h"
#include "log_cloud.h"   // logcloud::*, log_cloud_on_setting()
#include "ws_bridge.h"   // the real declaration of ws_push_hub_caps()
#include "remote_allow.h"
static const char* TAG = "api_system";
#include "system_apply_settings.inc"
#include "remote_hooks.inc"
