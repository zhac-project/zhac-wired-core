// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Advertise <hostname>.local plus an _http._tcp service on port 80.
//
// On a wired box there is no SoftAP and no provisioning page, so this is
// the entire discovery story: plug in, take DHCP, be findable. Failures
// are logged loudly rather than swallowed -- if this does not come up the
// device is only reachable by an IP address the user has to hunt for.
//
// Call after eth_start(). Safe to call before the HTTP server exists; the
// advertised port is fixed at 80 and nothing dials it until a client does.
void net_discovery_start(const char* hostname);

// The hostname actually in use, without the .local suffix. Empty until
// net_discovery_start() has succeeded. Exposed so the status API and the
// boot log can report the same string.
const char* net_discovery_hostname();
