<!--
SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Changelog

All notable changes to `zhac-wired-core` are recorded here. Format follows the convention
used across the other ZHAC repos: an `## [Unreleased]` section accumulates work, and its
contents become the release-tag annotation at `just release`.

## [Unreleased]

### Added

- **Weekly schedule for Saswell SEA801/SEA802 radiator valves** (and their white labels): the device page's States tab shows a Monday-to-Sunday editor, four periods a day (start time + °C), with "Copy Monday to Tue–Fri" and one Save for the changed days. The valve reports each day and takes a day per write (embedded-zhc codec; www-spa editor, pinned in `release-manifest.json`). Home Assistant gets the seven days as editable text entities. The valve runs the program on the clock the hub sends it, so set the hub's timezone.

### Fixed

- With more than roughly 25–35 devices, the device list stopped short without a word: `device.list` wrote every row into one 8 KB buffer and quit when it was full, so ZHAC Cloud never learned about the remaining devices (they could not be shown, used in automations or picked for a panel) and the local web UI's device table was cut off too. The web UI now gets the whole list in a buffer sized to the pool. ZHAC Cloud gets it in pages of at most 8 KB — `{"items":[…],"next_cursor":"0x…"}` in IEEE order, `next_cursor` left out on the last page — which the cloud's reconcile already follows; the IEEE cursor stays valid when devices join or leave between two pages (host test `main/test/host`).
- The hub rebooted every few hours: `ws_server_broadcast` (from zhac-net-core) asserted that only one task broadcasts, but here the event bus, the status tick and the log sink all do; two at once aborted (`assert failed: ws_server_broadcast … s_broadcast_task == nullptr`, read from the crash dump). The send loop is now serialized.
- Every device command the library encodes (switching, setpoints, Tuya writes) left this hub with ZCL transaction number 0: the encoder leaves a placeholder for the platform to fill, the ZNP bridge fills it, the esp-zigbee backend sent the frame as it came. Tuya devices take a frame carrying the number of the one before for a resend of it, and a Saswell valve given Monday–Friday in one Save echoed Tuesday–Friday with a different program than the one saved. Commands now take a number from the shared ZCL counter; replies built by the backend (genTime, default responses) keep the request's number.
- Configure or a setting change on a sleepy Tuya thermostat (Saswell SEA801) did nothing: the query or write waited ~7.7 s in the coordinator's indirect queue and expired before the valve polled (it checks in every ~20 min), so the schedule was never read and a new setpoint never arrived. Such frames are now held and resent when the device next transmits (shared `zhc_adapter` wake queue). The backend tells the adapter which devices sleep from the neighbour table (`rx_on_when_idle`), or from a battery power source for devices parented by a router. The log shows `[wake] … held for its next wake-up` and `[wake] … is awake -- resent …`.
- The v2026092205 release images were built against the previous `zhac-components` / `embedded-zhc` pins in `release-manifest.json`, so they lack the Tuya time-sync answer (0xEF00 cmd 0x24) that release lists. The pins now match the sources the bench build used; flash v2026092206 or later.
- The hub could not connect to ZHAC Cloud at all: the cloud client (`remote_client`, pulled from zhac-net-core) is switched by `CONFIG_ZHAC_REMOTE_CLIENT_ENABLE`, which only the net-core and mono Kconfig trees define, so every wired build compiled it out and the web UI hid the Remote card. The wired tree now defines the "ZHAC Remote (cloud link)" menu (on by default; the link stays idle until a URL and token are saved). With it on, three more faults showed: the client waited for a Wi-Fi got-IP edge that an Ethernet hub never produces after boot (fixed in net-core `remote_client`); every event reached the cloud wrapped twice (`{"event":..,"data":{"event":..}}`), so the cloud found no device in it; and events were mirrored only while a browser tab was open. Pushes now hand the relay the bare payload and go out whenever the relay is connected. Cloud commands run the same handlers as the local WebSocket, so the remote task gets the httpd worker's 12 KB stack (`CONFIG_ZHAC_REMOTE_TASK_STACK_KB`). Ported from net-core's `api_remote`, which the port had dropped: the link URL must be `wss://` (DS9: a `ws://` link would carry the token and device control in cleartext), URL/token/device id are length-checked (an over-long value used to save fine and then fail to load at every boot), and `remote.status` reports the state by name, so the Remote card shows "Connected" instead of a bare number.
- A sleepy Tuya thermostat (Saswell SEA801) showed only `local_temperature`: every bind, read and DATA_QUERY to it failed with `apsde_data_request … failed (1)` (the stack's buffer pool spent by frames still pending for the sleeping device), and the interview loop downgraded its late-path identity to unknown. Sends from a task now wait up to 4 s for buffers instead of failing at once; a Basic-read burst stops when the stack refuses to queue one; a device whose identity is already known is not read again; a device the decoder matches while the pool still says unknown is marked identified+matched on the spot (so the configure-while-awake kick fires); bind answers get 10 s (z2m's ZDO timeout) instead of 5 s.
- A re-paired battery device could stay without manufacturer/model: with several sleepy devices cycling through retries (each failed attempt is ~40 s of timeouts) the newcomer waited minutes for the single interview task and was asleep again by its turn. An announce now preempts the running attempt within 200 ms, so the device that just announced is interviewed while it is awake; the interrupted one goes back into the queue.
- A device that is joined but unknown to ZHAC (settings wiped, or it joined while the firmware was down) was never interviewed: joined devices do not announce again and this backend only created pool entries on an announce, so mains devices stayed "not in the pool" with no model. A frame from such a device now creates its entry and queues an interview, as the ZNP path does.
- Installing from the browser flasher wiped devices, rules and settings even without "erase": the merged release image pads the gap between partition table and OTA data with 0xFF, and that gap is the NVS partition. Releases now also publish the four parts (bootloader, partition table, OTA data, app); the flash page and the esptool instructions write only those regions.
- One press of a Tuya TS0044 remote arrived twice (`action = "1_single"` × 2, toggling a relay on and off): Tuya sleepy devices resend a command when no ZCL Default Response comes back and this backend answered nothing. Every unicast command that did not opt out is now acknowledged with a Default Response (the ZNP path's rule), and an identical resend within 1.5 s is dropped.
- A groupcast-only zone remote (MiBoxer FUT089Z, zones = groups 101–108) showed only battery and voltage: the coordinator was in no group, so its presses never arrived. The backend now registers endpoint 1 with Basic + Groups servers, joins groups 101–108 when the network is up (and every group a device is added to through ZHAC), reads its own membership back at boot (`coordinator endpoint 1 is in N group(s)`), and hands the groupcast's group id to the decoder so `zone` is synthesised. This is the "rank 1" path from `NATIVE_ZCL_GROUPS_DESIGN.md`; the TI ZNP path has no equivalent.
- MQTT published nothing after "connected" unless Home Assistant discovery was on: device updates only went through the HA bridge. Every update is now also published on `<root>/devices/<IEEE>/state` as `{"ieee","attrs":{key:value}}`, as the dual-chip S3 does.
- Saving a Lua script from the web UI failed with "Unsupported" (HTTP 405): the SPA and net-core save with `POST /api/scripts/<name>`, this port accepted only `PUT`. POST without `/run` or `/check` now saves.
- `groups_store` brought up to net-core's version: one recursive store mutex (created at boot via `grp_store_init()`), `grp_create()` allocates the id and saves under the lock (two concurrent creators could take the same slot), `group.list` holds the lock across its shared buffer. The port had carried the pre-lock copy.
- `zap_store_flush_now` is registered as a shutdown handler (as on the P4), so device-store writes still pending in the writeback cache survive a reboot from the UI or an update.
- A bind the device rejected (ZDP status other than success, e.g. binding table full) aborted the whole configure, so the steps after it never ran: a TS004F knob (ERS-10TZBVK-AA) never got its Tuya magic packet or the `operation_mode=event` write and stayed silent on this backend while it worked on the P4, whose ZNP path does not wait for bind answers at all. A rejected bind is now a warning (with the ZDP status) and configure carries on; a timed-out bind still fails and is retried when the device wakes.
- Configure reporting for 5–8 byte analog attributes (u40/u48/u56/u64, e.g. seMetering `currentSummDelivered` u48 on TS011F plugs) was rejected as an unsupported type, which failed the whole configure on every attempt. The pool's short address is also cross-checked against the stack's address map on every frame: a stale entry that still held an address a newcomer now uses made the newcomer's frames decode under the old device.
- A frame from a short address the pool did not know (a device that rejoined under a new address without an announce we saw) was dropped with only a diagnostics counter, so the device looked silent. The APS hook now resolves the IEEE through the stack's address map, refreshes the pool's short address and decodes the frame; genuinely unknown sources are logged (rate-limited).
- Configure (bind to the coordinator, reporting, Tuya magic / mode writes) of a sleepy end-device failed on every timed retry (1/5/30/120/600 s, then "giving up until rejoin"): each attempt had one 5 s window and the device was asleep. Any frame from a matched device whose configure is not DONE now re-queues configure while it is awake (rate-limited per device), and a rejoin announce re-queues it too, as the ZNP path's rejoin fast-path does.
- Rules were never stored: `main.cpp` never called `rule_store_init()` / `rule_store_flush_init()` (only the P4 main-core did), so the NVS namespace was never opened, every save failed silently and the UI still said "Rule saved" with nothing listed. Both calls added before `simple_rules_init()`, plus the shutdown flush.
- The coordinator answers `genTime` reads (attributes 0x0000/0x0007, UTC seconds since 2000 once NTP has run, UNSUPPORTED otherwise). Tuya remotes (FUT089Z, TS0044) and plugs read it right after joining and keep asking until they get a value; the remotes arm their button reporting on it. Same responder the P4 has had. ZDO answers (profile 0) no longer reach the ZCL decoder (`decode_frame failed cluster=0x8021` noise gone).
- Joining a sleepy Tuya remote (MiBoxer FUT089Z) never completed, and while its interview looped (10 × 30 s) no other device could join: the single interview task slept between attempts, so a Xiaomi sensor announcing meanwhile waited in the queue until it gave up and left. Now mirrors the ZNP path: a failed Active-Endpoints step assumes endpoint 1 and goes on to the Basic read (five back-to-back reads keep a frame queued for the sleepy child), any frame or rejoin from the device under interview ends the 30 s wait at once, and when another device announces the current one goes back into the queue and the newcomer is interviewed first.
- S31: internal DRAM exhausted ~10 s after boot (228 KB → 5 KB: Zigbee init 63 KB, Lua 57 KB, Ethernet 30 KB, httpd 22 KB, then task stacks) so `mqtt_client: Error create mqtt task` and any later task (OTA) failed. Task stacks + queues now live in PSRAM (`zhac_task.h`, TaskEventBus, TaskZigbee, TaskZbIv, httpd via `task_caps`), Lua small allocations go to PSRAM (`LUA_ENGINE_INTERNAL_SMALL_THRESHOLD=0`), mDNS task/memory, MQTT outbox/buffers and mbedTLS allocate from PSRAM. Boot log prints `int-heap after <step>` per init step.
- **`TaskEventBus` no longer burns a fifth of core 0 while idle**: the pump sleeps until a
  publish instead of polling every 20 ms (shared `event_bus_pump_run`).

- **String attributes showed the previous push's JSON text** (first seen on an Aqara
  WXKG01LM button: `action` read `{"event":"attr.changed",...`). The shadow's string field
  holds up to 48 bytes with no terminator; the WebSocket push and the device state built the
  JSON straight from it, so ArduinoJson read on into the stack. Every site copies into a
  terminated buffer now.

- **Boot loop on the first live boot of the DIY-pass firmware.** `ntp_cfg_init()` (the
  DHCP time-server switch) ran before `eth_start()`, i.e. before the TCP/IP thread existed,
  and `esp_sntp_servermode_dhcp()` asserted in `tcpip_callback` two seconds into every boot.
  It now runs right after `eth_start()`, still well before the first lease lands.

- **S31: the web UI had no live data — every WebSocket handshake failed.** IDF v6.1-beta1
  hashes `Sec-WebSocket-Accept` through PSA, its mbedtls port drops the software SHA-1 when
  the hardware SHA driver is on, and that driver cannot set up SHA-1 on the S31
  (`httpd_ws: Failed to setup SHA-1 operation` on every connect; `ws_clients` stayed 0, the
  Info page showed only its Help card). `sdkconfig.defaults.esp32s31` now sets
  `CONFIG_MBEDTLS_HARDWARE_SHA=n`; the P4 builds on IDF 6.0 keep both and are unaffected.

- **Status JSON carried stack garbage as the MQTT client id**, so the web page could not
  parse `/api/status` and showed the hub as unreachable. ArduinoJson keeps a `const char*` by
  reference; the status builder handed it a field of a `const` local that died before
  serialisation. It copies now. Broker URL, root topic and client id read from NVS are also
  checked to be printable ASCII (an empty client id falls back to the MAC-derived default).

### Changed

- **MQTT settings handling moved to the shared `mqtt_gw_cfg`** (zhac-components); same
  behaviour, one implementation with the single-chip build.

- **Home Assistant: thermostats, covers, locks, fans and buttons become their own entity
  types**, and battery devices turn unavailable after a day of silence (zhac-components
  `ha_bridge`). Soft-deleting a device now removes it from Home Assistant too.

- **Rule pushes come from the rule engine, not the transport.** `rule.added` / `rule.updated`
  / `rule.deleted` are built from the `RULE_CHANGED` event, so a rule created, edited, toggled
  or deleted over REST or by a backup restore now updates open Rules pages and the cloud relay;
  before, only WebSocket edits did.

- **Device rename, delete and permit join go through `device_cmd`** (zhac-components). Rename
  now reloads the rule engine's name table (before, a renamed device silently stopped matching
  its rules until reboot) and still tells Home Assistant. Delete has one contract: soft asks
  the device to leave and hides it (name kept for a rejoin); `hard` also wipes the pool slot,
  shadow, converter caches and stored row. Before, soft only dropped the pool entry, so the
  device stayed joined and came back. The join-window deadline moved out of `radio_state`
  into the shared service; `zigbee_leave_req` is now a real backend entry point.

- **Every attribute write goes through `device_cmd`** (zhac-components): REST, WebSocket,
  MQTT / Home Assistant commands and collection fan-out now share one implementation, so they
  accept the same value types, answer with the same words, and all mirror the command into the
  shadow (collection fan-out and REST did not before).

- **Storage faults no longer erase the owner's data.** When the NVS partition cannot be
  initialised at boot (no free pages, format version change) the hub used to erase it
  silently: devices, rules, names, passwords gone. It now boots locked and empty instead:
  sign-in forced on with a serial-only token, status `storage_error: true`, and Settings offers
  "Erase storage and restart" (`system.storage_reset`, WebSocket) so the erase happens only
  on the owner's word. Architecture review A3.

### Fixed

- **Architecture review (2026-09-20) quick fixes.** Release manifest pins the pushed
  `feat/diy-readiness` commits (the old pins lacked `ha_bridge`, so a tagged build could not
  configure). Sign-in fails CLOSED when its storage cannot be opened: token in RAM, printed on
  the serial console, password set-up refused (`503 storage_error`, status
  `auth_storage_error`) — it used to boot with sign-in off. `GET /api/devices` copies the pool
  under its lock and sends from the copy; the REST attribute setter releases the lock before
  the radio dispatch. The REST device list carries decimals (`VAL_FLOAT` ÷ 100). The
  post-update health check also requires the event dispatcher task and readable sign-in
  storage.

### Added

- Releases now ship the ESP32-S31 image (`zhac-wired-s31-<tag>.bin`, offset 0, plus the `-ota.bin`) built on IDF v6.1 next to the P4 image; CI builds the S31 on every push. The browser flasher at zhac-project.github.io/zhac-docs/flash/ installs it. README rewritten around the S31 board: buy, flash from the browser, open zhac.local.
- **Status LED.** The board's addressable RGB LED (`CONFIG_ZHAC_STATUS_LED_GPIO`, 60 on the
  S31 dev board, off on boards without one) blinks green while the join window is open and
  flashes blue on Zigbee traffic (a report, a raw frame, a join).

- **`diag.tasks` + a Tasks card on the Diag page**: every task with its CPU share over the last
  five seconds, the core it is pinned to, priority and stack headroom, so "core 0 sits at 25 %"
  gets a name. `CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y` supplies the core.

- **Time from the router.** When no time server is named, the hub asks the router for one
  (DHCP option 42) and uses it before `pool.ntp.org`; status reports it as `ntp_dhcp_server`
  and the Settings Time card says so. A hub on a network without internet access then keeps
  its clock across power cuts with no configuration. Rows of `device.list` carry `model_id` and `known`.

- **The first claim of a hub is bounded.** A hub without a password let the first visitor set
  one at any time, so an unclaimed or reset hub belonged to whoever found it weeks later.
  Set-up now answers only in the first ten minutes after power-on (`403 setup_closed` after,
  status `auth_setup_secs_left`); the web UI explains and asks for a power cycle. Shared
  helper `zap_setup_window.h` in zhac-components, so net-core behaves the same.

- **A time server you can choose** (Settings, Time; `settings.set {"ntp_server"}`). A hub
  without internet access can use one on its own network, so its clock returns after a power
  cut without anyone opening the web UI. Empty means the public default, and status reports
  the server as `ntp_server`.
- **`time.set {epoch}` for a hub without internet access.** No board has a battery-backed
  clock, so an offline hub had no time and its schedules never ran. The web UI now hands over
  the browser's clock when status says `clock_set: false`. The hub takes it only while its own
  clock is unset, so a browser never moves a clock that SNTP has set. WebSocket only.
- **Over-the-air updates.** WS `ota.update {url}` downloads a release's `-ota.bin` over
  HTTPS (server certificate verified) into the idle slot and reboots into it; the web UI's
  OTA page drives it and shows progress. With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` a new
  image that dies within its first minute is rolled back.
- **The web UI is embedded in the app image** (`tools/pack_spa.py`, served gzip-encoded by
  `spa_serve.cpp`, with a Content-Security-Policy header and cache headers that make an
  update show at once). An OTA update now carries firmware and UI together.

- **MQTT works on this build, and Home Assistant discovery with it.** The build used to
  link mqtt_gw's HAP shim — publishes went nowhere and the status always said disconnected.
  It now links the real client (`ZHAC_MQTT_GW_LOCAL_CLIENT`), keeps the broker settings in
  NVS `mqtt_cfg` (they used to vanish on reboot), starts it on Ethernet link-up, feeds
  inbound messages to `Mqtt#` rules and Lua `zhac.on_mqtt`, and runs `ha_bridge` for Home
  Assistant (Settings → MQTT → Home Assistant discovery).
- **A missing or wrong radio firmware no longer boot-loops the hub.** With no `ot_rcp` on
  the C6 (a new Guition board ships Wi-Fi co-processor firmware), OpenThread's spinel
  driver aborts during radio start. A boot guard in no-init RAM now spots that the previous
  boot died within 30 s of starting the radio, skips the radio on the next boot, keeps the
  device pool and web UI up, and reports `radio_error: "radio_crashed"` in status. A reset
  retries.
- **ESP32-C6 radio image.** `tools/build-rcp.sh` builds Espressif's stock `ot_rcp` with
  `rcp/sdkconfig.defaults` (spinel UART on the module's former SDIO traces) into one file
  for offset 0x0; releases attach it as `zhac-rcp-c6-<tag>.bin`. README documents flashing
  it once through the C6's own header — the P4 cannot, as neither the C6's UART0 pins nor
  its BOOT pin reach the P4 on this board.
- **Release images.** Every `v*` tag builds the P4 firmware and attaches one merged
  `zhac-wired-p4-rev1x-<tag>.bin` (flash at offset 0x0) plus `SHA256SUMS` to the GitHub
  release, with flashing instructions in the notes. The browser flasher in `zhac-docs`
  picks the latest one up. The `rev1x` in the name is the P4 silicon family it boots on.

### Changed

- **A new firmware is kept because it works, not because a minute passed.** After an update the
  image was marked good on a 60 s timer, so a build whose web server or radio never came up
  stayed. It is now kept only once storage answers, the web server is up and the Zigbee radio
  is no worse than before the update (an unplugged cable is deliberately not a reason to go
  back); unmet after ten minutes it rolls back by itself, and the previous firmware then
  reports why in status (`ota_rollback_reason`) and on the OTA page. Status carries
  `ota_state` (`pending` / `verified`), and a second update is refused during the trial.
- **Releases are rebuildable from the tag alone.** CI and the release build used to check
  every sibling repository out at its moving default branch, so an image could only be
  reproduced as long as those branches had not moved. `release-manifest.json` now pins each
  sibling to a commit, both workflows check them out there, `tools/bump_siblings.sh` moves the
  pins, and each release ships the manifest plus a `sources-<tag>.txt`. The release also writes
  the tag into `version.txt`, so the firmware reports the tag as its version instead of a bare
  hash (which made the browser flasher offer every release as an update), and it fails when the
  web UI is missing from the image rather than shipping a hub that answers "web UI not built".
- **`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT` moved to `sdkconfig.defaults.esp32s31`.** Only
  IDF v6.1 knows the symbol; on v6.0 it made CI's unknown-symbol check fail for the P4.

- **Partition table: two 6 MB OTA slots replace the single `factory` slot, and the 7 MB
  `spa` SPIFFS partition is gone.** Boards on the old layout need one full USB flash. The
  merged release image shrinks from 15.8 MB to about 4.5 MB.
- **The partition table moves from 0x8000 to 0xC000**, giving the bootloader 40 KB. With app
  rollback on, the P4 bootloader had 480 bytes to spare. `nvs` moves to 0xD000; the app
  slots stay where they were. Local builds: delete an old `sdkconfig` or run
  `idf.py reconfigure`, or the app will look for the table at the old address;
  `tools/check_resolved_config.sh` now catches that.

### Security

- **The REST and WebSocket APIs require sign-in, by default.** Ported from the dual-chip
  S3 (`auth.cpp`): a fresh hub asks the first visitor for the admin password
  (`/api/auth/setup`), the password is exchanged for the 32-hex API token
  (`/api/auth/login`), every REST route except `/api/status` and the login pair checks
  `X-Api-Key`, WebSocket clients must send a first `auth` message, and five failures a
  minute lock a peer out. Before this the whole API — Lua, rules, device control, reset —
  was open to anyone on the LAN (review 2026-09 tracked divergence). A stored "auth off"
  from Settings still wins; `CONFIG_ZHAC_API_AUTH_DEFAULT_ENABLED` sets the fresh default.

### Fixed

- **Decimal writes.** `device.attr.set`, `/api/device/state` and collection commands accept
  `21.5` (they answered "value must be bool / number / string"); the converter scales it, an
  integer-only converter refuses it as "no zhc converter", and the shadow mirrors it ×100.

- **CI and release builds failed on a clean runner.** `components/lua_engine` and
  `components/lua_cjson` compile `zhac-main-core`'s sources in place, and neither workflow
  checked that repo out, so CMake stopped at configure. Both workflows fetch it now, and the
  README lists it with the other siblings.
- **Status reports `clock_set`**, so the web UI can say when scheduled rules are waiting for
  the time (the rule engine now holds them until SNTP has set the clock).
- **A device's "last seen" always showed "—", and cron rules ran on a 1970 clock.** The
  board has no RTC and nothing ever set the time. The hub now starts SNTP (pool.ntp.org,
  as the dual-chip build does) on its first IP address, stores "last seen" as wall-clock
  time like the dual-chip build, and sends it with each attribute event so the web UI
  updates it live. Until the first sync it shows "—" as before.
- **The RCP UART pin prompts had the C6 side swapped.** The SDIO map puts C6 GPIO20 on
  P4 GPIO14 (the P4's RX), so the C6 transmits on 20, not 21. Values were unaffected; the
  help text now matches the image `tools/build-rcp.sh` builds. Unconfirmed on hardware.
- **The router listed the hub as `espressif`.** The DHCP request now carries the mDNS
  hostname (`zhac` by default), so the router's client list shows it — the fallback for
  phones that cannot resolve `zhac.local`.
- **The web UI showed raw IEEE addresses instead of device names.** `device.list` rows
  carried the name only as `friendly`; the shared UI reads `name`, as the dual-chip build
  sends. Both keys are sent now.
- **Temperature, humidity, power and every other decimal value never appeared.** Float
  attributes are stored ×100 and tagged `VAL_FLOAT`; `device.get` and the `attr.changed`
  push only handled int/bool/string, so they were dropped. They are divided back at the JSON
  boundary now, as the dual-chip encoder does.
- **Settings showed every toggle off and the MQTT fields blank.** WS `status.get` sent only
  diagnostics. It now shares one builder with REST `/api/status` and adds `sku: "wired"`, so
  the UI can also hide Wi-Fi and cloud-uplink controls this build does not have.
- **A device name containing a quote blanked the Devices page** (the list is built with
  `snprintf`, not a JSON writer). `device.rename` now rejects quotes, backslashes and control
  characters.
- **`GET /api/rules` overflowed the HTTP worker stack by 11×.** `handle_get_rules`
  kept `RuleSlot slots[ZAP_MAX_RULES]` — 256 × 536 B = 137 KB — as a local on
  the 12 KB httpd worker, on the unauthenticated REST surface; the frame
  overflowed on entry. The slots now come from PSRAM (internal heap as the
  fallback), the same allocation `cmd_rule_list` in `ws_bridge.cpp` already used.
  Same fix in zhac-mono-core, which carries the identical copy. (Review 2026-09,
  WC-02.)
- **The esp-zigbee SDK lock was never taken.** The lib makes
  `esp_zigbee_lock_acquire()` mandatory around every SDK call made outside a
  stack callback; this backend had zero call sites while issuing APS sends, ZDO
  requests, bind/unbind, permit-join and formation from httpd, the interview
  task, the configure worker, the event-bus task and the main task against the
  running mainloop. New `esp_zb_lock.h` wraps the lock in an RAII guard that is
  a no-op on the stack task (callbacks already hold it), re-entrant per task,
  and bounded (2 s — a wedged stack surfaces as a failed request, not a wedged
  httpd worker). Applied at the APS send funnel (`esp_zb_af_send`), the three
  ZDO interview requests, bind/unbind, Mgmt_Leave, open/close network, the PAN
  probe and the explicit formation/initialisation kicks. Locks are released
  before every wait on a step/bind semaphore, since the callbacks that post
  them need the lock to run. (Review 2026-09, WC-01.)

Initial firmware. The `esp32s31` target runs on hardware (network formation, pairing
and decode of a real device, ZCL groups, permit-join, live web UI); the `esp32p4`
target builds but has not run on hardware yet.

### Changed (docs)

- **README rewritten for first-time users**: per-target status that matches reality,
  a board matrix, a silicon-revision check to run before flashing a P4, and a first-boot
  section (`http://zhac.local`, permit join). The old text still described Phase 0
  ("no radio, nothing verified").

### Changed

- **Device library brought current with zigbee-herdsman-converters v26.101.0.**
  `embedded-zhc` is resolved by sibling path (`EMBEDDED_ZHC_PATH`), not pinned, so this
  is a rebuild rather than a code change here — but it is worth recording what the
  firmware now carries, because two of the changes alter decode behaviour for devices
  this board already talks to:
  - **A Tuya on/off datapoint wire-typed as ENUM now decodes.** The datapoint decoder
    previously required the raw value to be exactly `Bool` and silently dropped anything
    else; Tuya firmwares ship the same logical datapoint as BOOL on one batch and ENUM on
    another. Nine definitions covering TRV601 / TRV602 / TS0601_thermostat_1 were affected.
  - **`action_duration` is no longer suffixed by endpoint** on multi-endpoint devices, so
    it arrives under the name the definition and z2m both use.
  - Two Mazda TRV definitions were reading one element past the end of their datapoint
    table on every lookup miss.
  - 42 new device definitions across the v26.99.0 and v26.101.0 windows; the library is
    now 5847 definitions over 387 vendors.

  Both targets rebuilt clean against it — `esp32s31` (IDF v6.1-beta1) at 0x37e1c0 bytes,
  42% of the app partition free, and `esp32p4` (IDF v6.0). Not yet re-run on hardware.

### Added

- **Repo skeleton targeting `esp32p4`** — 16 MB flash, single `factory` app slot, no
  `phy_init` partition (it holds Wi-Fi PHY calibration data, meaningless in a build with no
  radio on the host). PSRAM mandatory, with `SPIRAM_RODATA` and external BSS placement.
- **Chip configuration taken wholesale from `zhac-main-core`** — the config validated on
  ZHAC P4 hardware. All eight chip-critical settings now resolve identically: revision
  family, `SPIRAM_MODE_HEX` @ 200 MHz, 360 MHz CPU (IDF would default to 400), QIO flash
  @ 40 MHz (IDF would default to DIO @ 80 MHz), 16 MB.
  - **`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_0=y`.** P4 has two
    incompatible revision families — v0.x–v1.x and v3.x — and that first symbol picks
    which one the binary targets. A binary built for one will not boot on the other.
    Accepted range is v0.0–v1.99, covering both ZHAC P4 boards (Guition
    JC-ESP32P4-M3-DEV v1.x, confirmed 2026-08-14; WT0132P4-A1 bench rig v1.3), so one
    build serves both. Neither is targetable by the v3.x family at all.
  - Setting `REV_MIN_0` **without** the gate is silently useless: kconfgen knows the
    symbol but cannot select it, so it drops to the default `_301` with **no warning at
    all**, not even "unknown kconfig symbol". Verified by experiment.
- **`tools/check_resolved_config.sh`** — asserts what the config *resolved to*, not what
  was requested. There are two ways a `sdkconfig.defaults` line can do nothing and only
  the first is noisy: an unknown symbol (warns) versus a known-but-unselectable one
  (silent). Grepping build output catches only the former. Verified both directions.
- **Wired Ethernet (`eth.cpp`)** — internal EMAC plus an external IP101 PHY over RMII, with
  `esp_netif` and the DHCP client. Exposes `eth_start()`, `eth_link_up()` and
  `eth_get_status(NetStatus*)`.
  - The PHY power-enable GPIO is driven high and allowed to settle *before* the PHY is
    probed. Skipping this makes SMI reads return all-ones and PHY detection fail with a
    misleading "no PHY found".
  - `REF_CLK` is sourced from the PHY (`EMAC_CLK_EXT_IN`). It must stay that way: with PSRAM
    enabled the EMAC and PSRAM share the MPLL, and at 80 MHz PSRAM speed no integer divisor
    produces 50 MHz within tolerance, so `EMAC_CLK_OUT` fails EMAC init outright.
  - `NetStatus` carries link, IP, netmask, gateway, MAC, speed and duplex — and
    deliberately no SSID or RSSI field, so Wi-Fi assumptions cannot survive in callers.
- **Board pins as Kconfig** (`Kconfig.projbuild`) for the Guition JC-ESP32P4-M3-DEV. The
  Phase-1 radio pins are declared now, unused, so the board wiring lives in one place.

- **mDNS discovery** (`net_discovery.cpp`) — `<hostname>.local` plus an `_http._tcp`
  service record. Hostname is `CONFIG_ZHAC_MDNS_HOSTNAME` (default `zhac`); two units on
  one LAN collide and mDNS renames one, so the boot log is authoritative, not the config.
  A failed service record degrades to name-resolution-only rather than aborting.
- **The mono-core control surface**, ported: REST (`/api/devices`, `/api/rules`,
  `/api/scripts`, `/api/system`, `/api/groups`, `/api/remote`, `/api/status`), the WS
  bridge, the SPA catchall, `sys_state`, `log_ring`, `device_options`, `groups_store`,
  MQTT and the metrics exporter — plus the `hap_master`/`hap_slave`/`mono_bridge` shims
  that turn the dual-chip SPI link into direct calls.
- **`components/zigbee_pool`** — compiles just `zigbee_pool.cpp` and
  `zigbee_diagnostics.cpp` out of the sibling `zigbee_mgr`, giving a real device pool and
  diagnostics ring with no ZNP in the binary. `zigbee_mgr` itself REQUIREs `znp_driver`
  and makes ~41 direct `znp_*()` calls, so it cannot be pulled in whole; those two files
  have zero.
- **`/api/net/status`** — wired-native link, speed, duplex, IP, gateway, MAC, hostname —
  alongside a `/api/wifi/*` compatibility shim so the shared `www-spa` runs unmodified.
  The shim reports `mode="eth"` with an empty SSID and `rssi=0`, `scan` returns an empty
  list, and the write routes answer **501** (not 404: 404 reads as "wrong route", 501 as
  "this build cannot do that"). Mirrored on WS as `net.status` plus `wifi.*` aliases.
- **`radio_state.{h,cpp}`** — `radio_present()` / `radio_ok()` derived from the
  `device_backend` registry, replacing mono's direct `zigbee_mgr_crashed()` calls. Both
  are false in Phase 0 and start reporting the real backend in Phase 1 with no change at
  the call sites.
- **`tools/check_sku_invariants.sh`** — makes the three deliberate omissions
  build-breaking. Matches on usage (`#include`, symbol calls, `CONFIG_` symbols) and
  strips comment lines first, so the comments documenting each absence do not trip the
  guard explaining them. Verified in both directions: passes clean, and catches a planted
  `#include "esp_wifi.h"` / `znp_driver_init()`.
- **CI** — sibling checkouts laid out side by side, the guard, an `esp32p4` build, and a
  job that fails on `unknown kconfig symbol` (see below).

- **Shaped for a second SoC**, ahead of an ESP32-S31-solo SKU (its own 802.15.4 radio
  replacing the C6, RGMII instead of RMII). Pure refactor of the P4 build — the resolved
  `sdkconfig` is **byte-identical** to before it, and the only binary delta is +290 bytes
  of `.text` from splitting a translation unit, with **no DRAM or PSRAM change**.
  - `sdkconfig.defaults` split into a target-neutral base plus
    `sdkconfig.defaults.esp32p4`, which ESP-IDF applies automatically afterwards
    (`tools/cmake/kconfig.cmake:182`). The base file must exist even if empty — per the
    IDF docs the per-target file loads *"if and only if"* it does.
  - Ethernet split into target-neutral plumbing (`eth_common.cpp`: netif, DHCP, events,
    status) and one board implementation behind `board_eth_new()` (`board_eth.h`), picked
    by `IDF_TARGET` in `main/CMakeLists.txt`. Selecting by file rather than by `#if` means
    a build for one target cannot compile the other's code. An unrecognised target fails
    at CMake time with a message naming what to add.
  - `eth_get_status()` now reports gigabit link speed rather than collapsing anything
    non-100M to 10 — the S31 has a gigabit MAC.
  - CI is a target matrix with the IDF version pinned **per row**; the S31 row is present
    but commented, with the two toolchain blockers recorded.

### Notes

- **No radio in this phase, by decision.** The Zigbee stack arrives in Phase 1 as
  `esp_zigbee_backend` (`esp-zigbee-lib` on the P4 in `ESP_ZIGBEE_RADIO_MODE_UART_RCP`,
  driving a C6 running stock `ot_rcp`). Building the legacy CC2652/ZNP path here would mean
  debugging and then discarding a radio this SKU will never ship.
- **No Wi-Fi, ever, in this repo** — `esp_wifi` is absent from every `REQUIRES` list.
- Supersedes the 2026-05-27 Path-A decision in `extra/docs/ZNP_API_SURFACE_C6_PORT.md`
  **for this SKU only**; that record still governs the flagship line.
