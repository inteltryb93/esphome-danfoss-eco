# Danfoss Eco eTRV — merged ESPHome component

This component is a merge of two community forks of
[`dmitry-cherkas/esphome-danfoss-eco`](https://github.com/dmitry-cherkas/esphome-danfoss-eco),
brought up to date for **ESPHome 2026.6.3** on the **ESP-IDF** framework.

## Sources

| Repo | Commit (HEAD) | Common base |
|------|---------------|-------------|
| upstream `dmitry-cherkas/esphome-danfoss-eco` | `15b30a5` | — |
| `ryssel/esphome-danfoss-eco` | `e8491af` | `15b30a5` |
| `ckoca/esphome-danfoss-eco` | `ac72c9d` | `15b30a5` |

Both forks branch from the same upstream commit `15b30a5`.

## What each fork contributed

**ryssel** targeted ESPHome ~2026.1 and reworked the runtime/BLE layer:
- Rewrote XXTEA inline (dropped the external `xxtea-iot-crypt` library).
- `device.cpp` safety hardening: null-checks before casting `DeviceProperty::data`, 5–30 °C
  range validation, write-only-on-change, and a guard against writing mode + temperature in the
  same connection cycle (the "switching heating↔idle bricks the eTRV" fix).
- Removed the manual `esp_ble_gap_stop_scanning()` (it blocked a *second* eTRV from connecting).
- Migrated `ClientState::READY_TO_CONNECT` → `BLEClientBase::connect()`, `ClimateTraits` to the
  new feature-flag API, `ESPBTUUID::to_string()` → `to_str()`, `address_str()` (now `const char*`).
- Added raw-BLE-before/after-decryption debug logging.
- Replaced the removed `esp32_ble_tracker::Queue` with a FreeRTOS-queue wrapper.

**ckoca** targeted ESPHome ~2025.11 and focused on packaging + features:
- Vendored the original boseji XXTEA library into the component
  (`xxtea-lib.h`, `xxtea_core.h/.cpp`), removing the `lib_deps` requirement.
- Replaced the removed `esp32_ble_tracker::Queue` with the official `esphome::LockFreeQueue`.
- Added a `problems_detail` **text sensor** with human-readable fault strings.
- Migrated `climate_schema()` / `binary_sensor_schema()` codegen.

## Per-file merge decisions

| Area | Decision | Why (verified against ESPHome 2026.6.3 source) |
|------|----------|------------------------------------------------|
| **XXTEA** (`xxtea*`) | **ckoca** vendored boseji lib + wrapper | The XXTEA core is byte-for-byte identical in both forks (same `DELTA`/`MX`/rounds). ckoca's wrapper copies into an **aligned `uint32_t` staging buffer**, avoiding the unaligned-32-bit access that ryssel's `(uint32_t*)data` cast risks on Xtensa, and keeps **status codes** for logging. |
| `helpers.cpp` | base + **bug fix** | Fixed `(size_t*)&value_len`: `value_len` is `uint16_t`, but `Xxtea::encrypt` takes `size_t *maxlen` and writes `*maxlen = l*4` → a 4-byte write through a 2-byte pointer = out-of-bounds stack write (UB). Replaced with a real `size_t out_len`. ckoca left this bug in place; ryssel only removed it incidentally via its API change. |
| `device_data.h` | **consensus** (both forks) | `WritableData(uint16_t l,…) : DeviceData(8,…)` → `DeviceData(l,…)`. With the hard-coded 8, 16-byte settings were truncated (only first 8 bytes encrypted → "last 8 bytes garbage") **and** `pack()` overflowed the 8-byte stack buffer by 6 bytes. The write buffer is sized from the same `length`, so the fix is self-consistent. |
| `command.h` | **removed** (2026-09-27) | The one-shot command queue was replaced by pending-operation flags in `device.cpp` (see "BLE lifecycle rewrite" below). A queued command that had already been popped and sent could not be retried after a link loss, and the queue was the place where the stale-counter bug lived. |
| `device.cpp` | **ryssel** (superset) + honest logging | ryssel is a strict superset of ckoca's `connect()` change plus the safety hardening. Its removal of `esp_ble_gap_stop_scanning()` is correct on 2026.6.3: `BLEClientBase::connect()` sets the client to `CONNECTING`, and `ESP32BLETracker` automatically pauses scanning while any client is CONNECTING/DISCOVERED/DISCONNECTING — so the manual GAP call is redundant and desyncs the tracker. The combined mode+temp call now logs that the temperature is *ignored* (not silently dropped). |
| `device.h` | base + `<set>` (consensus) + ckoca `LOG_TEXT_SENSOR` + ryssel `address_str()` | `BLEClientBase::address_str()` returns `const char*` in 2026.x, so the old `.c_str()` no longer compiles (ckoca didn't fix this; ryssel did). |
| `my_component.h` | **merged** | `traits()` uses the real 2026.6.3 API (`add_feature_flags(CLIMATE_SUPPORTS_CURRENT_TEMPERATURE\|CLIMATE_SUPPORTS_ACTION)`, `add_supported_mode` ×2, `set_visual_*_temperature`). `set_supports_current_temperature()/set_supports_action()` were **removed**. Combines ryssel's configurable visual range (`set_temperature_range`, default 5–30 °C) with ckoca's `problems_detail` text sensor. |
| `properties.cpp` | **ryssel** (to_str + debug logging + `set_temperature_range`) + **ckoca** `problems_detail` block | The two fork changes touch different functions and merge cleanly. |
| `climate.py` | **merged** | `climate.climate_schema(DanfossEco)` (replaces the removed `CLIMATE_SCHEMA`; already declares the id, so the explicit `GenerateID` is dropped per ryssel). `binary_sensor_schema()`/`text_sensor_schema()` (`BINARY_SENSOR_SCHEMA` was removed). Adds ckoca's `problems_detail`. **ckoca's custom `visual:` block was dropped** — `_CLIMATE_SCHEMA` already provides a built-in `visual:` (min/max/step), so the custom one conflicted with it. |
| `danfoss_eco_scanner/*` | base, unchanged | Neither fork touched it; its `ESPBTDevice::address_str()` still returns `std::string`, so its `.c_str()` is fine. |

## Build / framework

Use the **esp-idf** framework (no `libraries:`/`lib_deps` are required — XXTEA is vendored):

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
```

See `test_merged.yaml` for a complete minimal validation config.

## BLE lifecycle rewrite (2026-09-27)

Investigation of "the ESP leaves a hanging BLE connection that drains the eTRV battery" (very low
signal, WiFi active). Verified against ESPHome 2026.6.3 (device firmware) and 2026.10.0-dev
(`esphome/` checkout), ESP-IDF 5.5.5 Bluedroid sources, and 15+ minutes of VERBOSE logs per phase with
scripted temperature changes (`aioesphomeapi`) on 4 eTRVs.

### Root cause found in the merged `device.cpp`

1. **Stale `request_counter_` → hung link.** The component only disconnected when
   `request_counter_ == 0` after draining the queue. The counter was decremented solely by
   `ESP_GATTC_READ_CHAR_EVT`/`WRITE_CHAR_EVT`. When the link dropped with requests outstanding (very
   likely at a poor signal: the eTRV runs a ~500 ms connection interval and a 6 s supervision timeout),
   Bluedroid delivers an error callback for at most the *in-flight* request; queued ones are freed
   silently (`bta_gattc_clcb_dealloc`). Nothing reset the counter. Because the `ble_client` stayed
   enabled (`auto_connect: true`), the tracker reconnected as soon as the eTRV advertised again, the
   node became ESTABLISHED with an empty queue and `request_counter_ > 0` — and never disconnected.
   Even the next `update()` could not fix it (+4 −4). The link stayed open for hours (until the next
   link loss, after which the same happened again). ryssel fixed the same bug independently in
   `277ad11` ("teardown ... pending=%u") three weeks after the merge base.
2. **No timeouts at all** — no request timeout, no link watchdog. ESPHome only bounds the *open*
   (20 s) and DISCONNECTING (10 s) phases.
3. **Direct `parent()->connect()` bypasses the tracker.** The merge note claimed the tracker pauses
   scanning while a client is CONNECTING; it does not — it only refrains from *starting* a scan. It
   stops the scan and sets `esp_coex_preference_set(ESP_COEX_PREFER_BT)` only on the DISCOVERED
   "promotion" path (`ble_device_base`/`bluetooth_connection` use exactly this to request a link).
   Measured on the ESP32-C3 over 21 min with the old code: 20 direct opens, 4 timed out (status 0x85
   after 20 s, reason 0x100 = the eTRV was never heard); 6 promoted opens, 1 timed out. Every failed
   open was rescued only by the tracker's `auto_connect` re-promotion (0.2–4 s after the eTRV was
   heard), i.e. the old code's recovery depended on `auto_connect` staying enabled by accident.
4. `on_write_pin` failure called `mark_failed()` — a PIN write cut short by a link loss (status 0x85)
   permanently killed the entity until reboot.
5. `DeviceProperty::handle` was uninitialised; a missing characteristic left garbage in it.

### What the rewrite does (see README "BLE connection lifecycle")

Pending-operation flags instead of a command queue; requests are issued in batches (PIN → writes →
reads) and counted per link (`inflight_`, reset on every CONNECT/CLOSE/DISCONNECT); `request_timeout`
(YAML, 15 s), 60 s link watchdog, `retry_window` (YAML, 10 min) with back-off 3/15/30/60/120 s (client parked between attempts after two failed opens, so a
thermostat at the edge of the range cannot keep blocking the others' serialised connects); links
requested via `parent()->set_state(DISCOVERED)` (tracker promotion: scan stop, coex PREFER_BT,
serialisation) with a direct-connect fallback if the tracker has not promoted the request within
15 s while no other eTRV is connecting (the tracker promotes one client at a time, so at boot the
four devices connect one after another); the client is parked at boot until the first poll
and disabled whenever nothing is pending; PIN failures are retried (3×) instead of `mark_failed()`;
handles initialised to `INVALID_HANDLE` and checked before use; mode and temperature writes go to
separate links; service discovery only on the first link after boot (then `V3_WITH_CACHE`, i.e. the
client reports ESTABLISHED at OPEN and issues no discovery), with automatic re-discovery after any
ATT error / unexpected value length / rejected PIN at the remembered handles, and value-length
validation before any payload reaches the fixed-offset parsers. Requested values live in the
`Device` (`requested_target_temperature_` / `requested_mode_`) and carry a version number: a write
only clears its pending flag if no newer value was requested while it was in flight, and the value
is applied to the characteristic data right before packing, so neither a fast second command (HA
slider) nor a read completing in between can drop a request. Compared with ryssel's later commits (`277ad11`, `8378c00`, `421fd64`): the request
watchdog with back-off and the counter reset are equivalent, the global connect slot is replaced by
the tracker's own one-at-a-time promotion, and ryssel's `auto_connect: false` advice is *not*
followed — the component now controls `enabled` precisely, and `auto_connect` is what recovers a
failed open the moment the eTRV is heard.

### Production hardening (2026-09-28, second pass)

Research (all forks, upstream issues, libetrv, etrv2mqtt, deltasystems-pl/danfoss-eco-ha, ...), two
adversarial multi-agent code reviews (26 + 10 findings, each verified by 3 independent skeptics) and
hardware tests on the owner's 4 eTRVs led to the following changes:

- **Read before every write, in the same link.** Mode writes change only byte 4 of the 16-byte settings
  block, set point writes only byte 0 of the temperature block; all other bytes are sent back exactly as
  read in that link (the old code packed from a copy up to one poll interval old, which could revert
  changes made in the Danfoss app).
- **Plausibility validation** of every decrypted payload; implausible data (wrong `secret_key`) is never
  stored and therefore never written back. `decrypt()`/`encrypt()` report failures (the old code parsed
  ciphertext as plaintext on a decrypt failure, and could send plaintext on an encrypt failure).
- **Requests are never dropped on a timer before 24 h**: fast phase (`retry_window`), then every 5 / 15
  min; per-request TTL; the last value wins; pending requests survive a reboot / OTA (preferences).
- **Commands before the first read are accepted** (the link reads first); de-duplication only against a
  pending request or a read younger than 60 s (the eTRV schedule or dial may have changed the value).
- **Optimistic publishing** of the requested set point / mode, replaced by the read-back value.
- **Protocol errors** (rejected PIN/request, implausible data, missing characteristics) counted, give up
  after 3 in a row; a 1-byte read payload is reported as "wrong pin_code / not paired".
- **Clock sync** (`time_id`): the owner's eTRVs had their clocks at January 2017 (E10). Without a valid
  clock the eTRV silently returns from SCHEDULED to MANUAL within minutes (observed twice); with the
  clock set, SCHEDULED persists and the schedule's set point is applied immediately. E10 itself stays
  raised on these units even with a correct clock, so an E10-only resync is limited to once per day and
  every clock write is a single best-effort attempt (at most once per hour).
- **Watchdogs**: 60 s link watchdog, request timeout (30 s for the first request of a link: PIN
  latency in V3 links measured median 2.0 s, p99 10.7 s, max 12.6 s), 90 s client-state watchdog that
  also closes a stale `ESP_GATT_ALREADY_OPEN` link or a lost OPEN.
- `connection` diagnostic binary sensor, `secret_key`/`pin_code` validated in YAML (an invalid hex char
  used to abort the firmware at boot), `update_interval` default 30 min, raw eTRV mode byte in the log
  (the owner's units are MANUAL (0), not VACATION as a log misreading suggested).

The "mode switch resets the valve" question (upstream #11): the cause was upstream's 8-of-16-byte
settings write (random decrypted settings: display flipped, "valve not installed", child lock, 55 C).
Verified on hardware with this component: 10 consecutive MANUAL<->SCHEDULED switches on one eTRV, 38
settings blocks read back, **zero changes outside byte 4**; 18 014 host test vectors byte-exact against
the Python reference implementation.

### Validation (2026-09-28, ESP32-C3, 4 eTRVs at a very poor signal, ESPHome 2026.10.0-dev / ESP-IDF 5.5.5)

`tools/ble_exercise.py`, final build (`sterownik-grzejnika-local.yaml`, DEBUG logs), 34 minutes:
every 90 s all four thermostats moved by +1 °C / back in parallel, each command followed 1.5 s later
by a second, different value ("double-tap", the last one must win), device rebooted twice by the
script, Home Assistant automation active in parallel.

| | |
|---|---|
| BLE links / closed by the component after a complete transaction | 71 / 71 (0 other closes) |
| link duration (median / p90 / max) | 2.3 s / 8.9 s / 11.4 s (the long ones: boot-time service discovery) |
| open attempts that timed out (status 0x85, eTRV not heard) | 35 of 85, all recovered by retries (9 × client parked) |
| commands verified against the published state | 56 verified, 1 "failed" = HA automation overrode a pending command (the component correctly applied the newer value) |
| verification latency (median / p90 / max) | 22 s / 74 s / 243 s (worst case: thermostat at the edge of the range behind other connects) |
| watchdog / request timeout / give-up / link loss / PIN failure / fallback connect | 0 |

Earlier runs the same night: old code (21 min, 23 links, 4 of 20 direct opens timed out, 0 hangs
observed), first rewrite (~30 min, 25 links, 10 opens failed and recovered), fast-path debug build
(12 rounds). One 5-minute retry window expired for the weakest thermostat before the default was
raised to 10 min (7 attempts, all 0x85 although the scanner heard the eTRV).

### Validation of the final 2026.9.0 build (2026-09-28, run 10)

`tools/ble_exercise.py --period 120 --delta 1.0 --double-tap 1.5 --restart-every 8 --unreachable
fake_thermostat`, 3 h (13:48–16:51), ESPHome 2026.9.0 / ESP-IDF 5.5.5, DEBUG + VERBOSE logs:

| | |
|---|---|
| commands sent (incl. double-taps) / verified by the eTRV read-back / failed | 586 / 251 / **0** |
| superseded by the Home Assistant automation (the newer value correctly won) | 40 |
| reboots by the script / API reconnects | 9 / 11 |
| BLE links to the 4 real eTRVs / closed by the component after a complete transaction | 329 / 325 (the other 4 ended by the reboots or a supervision timeout, all recovered) |
| link duration median / p95 / max | 2.7 s / 10.6 s / 16.3 s |
| watchdog / request timeout / give-up | 0 |
| unreachable (fake) eTRV | backed off to the slow phase (5 min) and parked, never blocked the others |

## Protocol audit against the official Danfoss app (2026-09-28, third pass)

The Android app (Danfoss Eco v1.5.0, from
[BenjaminSoelberg/danfoss-eco2](https://github.com/BenjaminSoelberg/danfoss-eco2)) was decompiled
(jadx) and every characteristic compared with this component (`CharacteristicReader`,
`CharacteristicWriter`, `BLEThermostat`, `Mode`, `Eco2Alert`, string resources). Findings and changes:

- **E10 is latched.** Bit n of the error characteristic is error code E(n+1). The eTRV raises E10
  "invalid clock information" after a battery change and never clears it; the app clears it by
  writing the error flags back without the bit (`writeAlertsToThermostat`) once the user dismissed
  its "battery changed" dialog. This is why the `problems` sensors of all four thermostats were
  permanently ON although three clocks were correct. The component now acknowledges E10 the same
  way once the clock is right (at most once per hour, only with the app's block layout).
- **Clock format.** The app writes `Date.getTime()/1000` (UTC) and `TimeZone.getRawOffset()`
  (standard-time offset); the eTRV adds the summer-time hour itself when its "automatic summer time"
  flag (settings bit 1) is set. Upstream projects (libetrv, deltasystems) and the previous version of
  this component wrote *local* time plus the current offset, which shifts the weekly schedule by
  2–3 h. Now app-exact; existing clocks are corrected automatically on the next link.
- **Modes 2 and 4 exist.** The mode byte is 0 manual, 1 schedule, 2/3 vacation, 4/5 pause (odd =
  return to the schedule afterwards). The previous plausibility check only accepted 0/1/3/5, so an
  eTRV put into a vacation from manual mode or paused from manual mode was reported as "wrong
  secret_key" and given up on; mode 5 was shown as HEAT and could not be left with HEAT. Now all six
  are accepted; pause maps to climate OFF (new, writable like the app: 4/5), vacation to AUTO. Like
  the app, no set point is written while the eTRV is paused.
- **All error codes.** E1–E15 (sensor, memory, hardware, motor E6, radio, encoder, battery) are now
  reported; previously only E9/E10/E14/E15.
- **Everything else exposed** (optional entities, see README): settings (mode, min/max/frost,
  vacation, all eight configuration bits incl. "automatic summer time"), PIN protection flag (the PIN
  itself is wiped after decryption), device name, weekly schedule (three characteristics, 20+12+12
  bytes), Device Information Service. Informational values are read once after boot (schedule once
  a day), only when configured, piggybacked on a link that has finished its work.
- Never touched, by design: bootloader (a write reboots the eTRV into firmware-update mode), update
  token, app language, the unknown 10010000 service, the Cypress OTA service.

Host test: 27 048 vectors (all six modes, error acknowledgment, clock, name, PIN flag, schedule parts,
wrong lengths) byte-exact against the independent Python codec; 0 of 3000 random blocks accepted as
a settings block.

Hardware results of the third pass (ESP32-C3, the four eTRVs, all firmware 2.08 / hardware B4 /
software 4.2):

- First link after boot: E10 was set on all four; the component set each clock in the app format,
  acknowledged E10, and the re-read showed **no errors** — the `problems` sensors went OFF. Three
  eTRVs keep their clock since (read back correct at every later link, E10 not raised again).
- **kanciapamamy does not keep its clock**: correct right after the write, 0 again within 1–6 min,
  with the old format, the new app format and the standard or the summer offset alike; it raises no
  error code for it. Its only configuration difference to the others is "automatic summer time" off;
  battery 57 % (lowest). Now reported as `clock_error` / `problems` = "Clock Not Kept". Irrelevant in
  manual mode; the weekly schedule and vacations cannot work on that unit.
- Pause on hardware: MANUAL → PAUSE_THEN_MANUAL (4) → MANUAL, settings block identical except byte
  4; while paused the eTRV reports its frost temperature as the set point and restores the manual one
  afterwards.
- Informational reads after boot: about 2 s once per eTRV (first link after boot ~12 s including the
  one-time service discovery); later links 2.2–3.4 s as before.
- Run 12 (previous build of this pass, VERBOSE, 33 min, 1 reboot): 46 commands verified, 0 failed,
  10 superseded by the Home Assistant automation, 58 links all closed after a complete transaction,
  0 watchdog / timeout / give-up.
- An independent code review of this pass found two issues, both fixed before the final build: a
  set point equal to the frost temperature, requested together with leaving pause, was de-duplicated
  against the paused (frost) value; and a leftover optional clock / E10 flag after a link loss could
  keep the retry ladder running (bounded). Optional work is now dropped at every link end and
  re-derived by the next read.

### Validation of the final build of the third pass (2026-09-28, run 13)

`sterownik-grzejnika-local.yaml` (= the production `sterownik-grzejnika.yaml` with the local
component; DEBUG logs, passive scanning, all optional entities), ESPHome 2026.9.0 / ESP-IDF 5.5.5,
`tools/ble_exercise.py --period 120 --delta 1.0 --double-tap 1.5 --restart-every 5`, 40 min:

| | |
|---|---|
| commands sent (incl. double-taps) / verified by the eTRV read-back / failed | 120 / 57 / **0** |
| superseded by the Home Assistant automation / still pending at the end | 3 / 0 |
| reboots by the script / API reconnects | 3 / 4 |
| BLE links / closed by the component after a complete transaction | 72 / 72 |
| link duration median / p95 / max | 2.7 s / 16.7 s / 19.8 s (the long ones: first link after a boot, with discovery and the informational reads) |
| failed opens (eTRV not heard for 20 s; salon 14, sypialnia 11, others 6 each) | 37, all recovered by the retry ladder |
| watchdog / request timeout / give-up / link lost mid-transaction | 0 |
| command latency median / p90 / max | 30 s / 101 s / 362 s |

Compile checks (ESPHome 2026.9.0, 0 warnings): ESP32-C3 production config; plain ESP32 with every
optional key and a time source; plain ESP32 without any time component (plus the scanner); the three
example configurations validate.

## Production deployment and link-time optimisation (2026-09-28, fourth pass)

Deployed on the user's ESPHome Device Builder (ESPHome 2026.9.0): the production YAML pulls the
component from GitHub (inteltryb93/esphome-danfoss-eco, master), credentials and eTRV MACs from the
builder's secrets, OTA password-protected. Built and flashed by the builder; 0 compiler warnings.

- **False client-watchdog alarm (fixed, 966be5b).** The watchdog stored its start time as `now | 1`;
  with an even `now` the start lay 1 ms in the future and two loop passes within one millisecond made
  `now - start` underflow. Seen once in the first production test (a normal connection attempt was
  reset and retried 3 s later). Now a separate "armed" flag.
- **Service rediscovery on every link (fixed, 9ea5d18).** Without Bluedroid's NVS service cache
  ESPHome clears the stack's service table after each disconnect, and the stack rediscovers all eTRV
  services at the start of the next link before our first request goes out. New option
  `cache_services` (default on, the same setting as bluetooth_proxy's): the table stays in flash; it is
  cleared before a discovery if the layout looked different, and before onboarding.

| link phase (4 eTRVs, weak signal) | before | with the cache |
|---|---|---|
| link open → first answer, median / p90 | 2.07 s / 7.9 s | 0.19 s / 0.31 s |
| whole link, median / p90 / max | 2.55 s / 10.7 s / 17.3 s | 0.70 s / 1.05 s / 5.2 s (a first link after boot) |
| first link after a reboot (with discovery + informational reads) | 15.6 s | 4.1 s |
| median command latency (parallel test) | 43 s | 19 s |

Other Bluetooth centrals: none of the user's ESPHome Bluetooth proxies holds a connection to an eTRV
(all connection slots free); two of them hear an eTRV (salon at -70 dBm, kuchnia at -99 dBm). Removing
them is not necessary; if they scan actively, switching them to passive scanning spares the eTRVs the
scan responses.

## Stress tests and the retry policy for failed opens (2026-09-29, fifth pass)

Test: `tools/scenario_test.py` on a VERBOSE build, all four eTRVs at their weak signal: parallel
(all four set points at once, then back), readonly (a re-send that forces a verifying read on all
four), same (two writes to one eTRV back to back), ab (A, B, A again, B again), double (a second
command 150 ms after the first), mixed (half write, half read at once), cold (first command after a
reboot). Every command is timed from the API send to the value read back from the eTRV. Home
Assistant writes its own set points every 10 min (at hh:m0, only when they differ); the test keeps
clear of that minute and follows a value HA sets.

**Connection establishment is the whole latency.** Once connected, a link takes 0.3-0.7 s (reads)
or 0.6-1.3 s (writes), PIN 0.1 s after the open. Over ~2900 connection attempts (all logs of this
work): 64 % connect within ESPHome's 20 s `connection_timeout`, median 6 s after the attempt starts.
The success rate of a running attempt is 4-7 % per second for the whole 20 s (a shorter timeout
would not help), it does not depend on how long ago the previous link to that eTRV ended (no faster
advertising after a disconnect), and waiting after a failed attempt does not raise it (55 % within
20 s after the failure, 61-66 % 20-200 s later; 57-63 % after 1-5 failures in a row; every eTRV
connected in the end, after at most 8). Per eTRV: kanciapamamy (-67 dBm) and kuchnia (-65 dBm)
74-76 %, sypialnia (-80 dBm) and salon (-88 dBm) 53-55 %: even at a strong signal a quarter of the
attempts fail, the eTRV advertises only every few seconds and the initiator listens half of the time
(Bluedroid's 30 ms / 60 ms direct-connect scan, which ESPHome's BLE client offers no way to change).

**Retry policy changed accordingly.** Before, a failed open was retried after 15 s, then 30 s, 60 s,
120 s (the eTRV "at the edge of the range" was not to block the others). A failed open costs the
eTRV nothing, so the radio idled for nothing: 40 % of the time during which work was pending. Now a
failed open is retried after 3 s, up to 6 times in a row, with the ble_client left enabled for
auto_connect; a thermostat that has just failed lets any other that is waiting go first (the
tracker promotes the first waiting client in its list, so this makes it round robin); only an eTRV
that fails more often than that gets the back-off ladder (15 s ... 2 min) and then the slow phase.

| same suite, same night (VERBOSE build) | old policy (back-off) | new policy |
|---|---|---|
| connection attempts that connected | 57 % of 136 | 64 % of 223 |
| **radio idle while work was pending** | **40 %** | **7 %** |
| parallel, all four done: median / max (6 rounds) | 80 s / 299 s | 86 s / 151 s |
| readonly, per thermostat: median / max | 52 s / 335 s | 25 s / 86 s |
| same (24 writes): median / p90 / max | 14 s / 80 s / 168 s | 13 s / 52 s / 57 s |
| a write after 1 / 2 / 3 failed opens | 33-52 / 75-83 / 168 s | 16-44 / 46-57 / 69-109 s |
| double: first value written although superseded | - | 0 of 12 |
| cold: first command after a reboot, median / max | - | 14 s / 56 s (first link 4.7-7.4 s) |
| timeouts / anomalies in the device log | 1 / 0 | 0 / 0 |

The old run was cut short in its ab scenario by a lost API connection (the test tool did not
reconnect yet). Not the controller: Home Assistant's history shows eight WiFi devices of the house
becoming unavailable within 40 s at that moment (a network-wide event, most likely the access
point), and the controller back in HA one second later without a reboot. The new run counted the
ESP's WiFi disconnects: none in 1.6 h (signal -64 dBm), no API connection loss, although the
Bluetooth initiator (which the tracker runs with the coexistence preference on Bluetooth) is now
busy most of the time while work is pending.

Monte Carlo with the measured attempt statistics (20000 runs): one command median 14 s both, p90
91 -> 52 s, p99 314 -> 102 s; four at once median 88 -> 76 s, p90 210 -> 138 s, p99 552 -> 209 s.

Answers on an established link (with the persistent service cache, 2682 of them tonight): median
65 ms, p99 0.3 s, never more than 0.65 s. Once in ~500 links an eTRV stopped answering right after a
set point write while the link itself stayed up (probably busy moving the valve): `request_timeout`
(15 s) closed the link and the next one read the new value back. The timeout stays at 15 s, because
without the service cache the stack's own discovery can delay the first answer by that much.

ESPHome's BLE client logs every open that is not answered within `connection_timeout` as
`[E] ESP_GATTC_OPEN_EVT in DISCONNECTING state (status=133)` + `[W] Connection open error` (the
stack reports the failed connection before the open result); documented in the README as expected
at a weak signal.

**A higher Bluetooth TX power does not help.** ESPHome leaves the BLE TX power of a client at the
ESP-IDF default, +9 dBm on the ESP32-C3 (+20 dBm possible, `CONFIG_BT_CTRL_DFT_TX_POWER_LEVEL_P20`).
A/B on the same night with a test firmware that switches the initiator and default TX power at
runtime (`esp_ble_tx_power_set_enhanced`, template buttons), alternating every 8 minutes while every
eTRV got a read-only link as soon as its last one was a minute old (`scenario_test.py --scenarios
txab`):

| connection attempts that connected | +9 dBm (default) | +20 dBm |
|---|---|---|
| all four eTRVs | 65 % of 158 | 57 % of 157 |
| kanciapamamy / kuchnia | 70 % / 86 % | 67 % / 74 % |
| salon / sypialnia (the weak ones) | 55 % / 49 % | 54 % / 36 % |

(With an earlier, round-based run: 62 % of 191 vs 57 % of 179, not a significant difference.) What
limits a connection is the ESP hearing the eTRV's sparse advertising, not the eTRV hearing the ESP;
the production configuration keeps the default. No WiFi disconnect and no API connection loss in
these 1.9 h either (WiFi -56...-64 dBm); the one request timeout described above was the only anomaly.

## Code review, fork survey and targeted tests (2026-09-29, sixth pass)

Two independent reviews of the component (time / state machine; data / memory) and a survey of all
22 forks of the base project (FORKS_REVIEW.md, section 7). Buffers, parsing, time arithmetic,
persistence and memory were found correct (all lengths checked before copying, no leaks, format
strings match, every millis() comparison wrap-safe). Fixed:

- **control() before setup() crashed** (null properties): an `on_boot` automation with the default
  priority runs before the component's setup. The properties are now created in the constructor; a
  command given that early wins over a request saved before the restart and is saved itself.
- **"Round robin" after failed opens was a fixed priority by list order**: only one eTRV can wait
  in the tracker, and the first due one in loop order took the slot, so with three or four eTRVs
  failing at once the last one got 3 attempts in 10 min against 9 for the others (simulated). Now
  first come, first served: the eTRV that has been due longest goes first (simulated: 8/8/7/7).
- **Links opened by auto_connect reused stale settings** (the connection type of the previous
  attempt): V3 is set as soon as a discovery has verified the handles, and the client is left
  enabled for auto_connect only when the next link needs no discovery and no cache clean.
- **A wrong `pin_code` cost a full service rediscovery on every poll** (7-11 s link instead of 0.7 s):
  a rejected PIN / request, or the eTRV's 1-byte error code, no longer forces a rediscovery when the
  handles came from a discovery answered by the eTRV itself (trusted for a day, so a changed layout
  after an eTRV firmware update is still found). Found via a fork that stops after 5 rejections.
- **Two climate publishes per read** (set point with the previous mode in between, e.g. the frost
  set point still with HEAT when the eTRV was paused at the dial): the climate state is published once
  per read batch (and when a link ends).
- **Texts from the eTRV are made valid UTF-8** (name, device information): a name cut in the middle
  of a multi-byte character or erased flash (0xFF) would have been sent as an invalid protobuf string,
  on which Home Assistant drops the whole API connection. Host-tested with 12 cases.
- A battery level above 100 is shown as unknown instead of counting as a protocol error (three of
  them used to make a transaction give up and drop a requested set point); `problems_detail` is cut
  at Home Assistant's 255 characters; the dynamic gauge range never exceeds what control() accepts;
  the "fresh read" check is wrap-safe (flag cleared by loop()); a late open result on an idle client
  is ignored; a re-sent identical pending value refreshes its TTL; the `shared_ptr` the properties hold
  to their component no longer owns it (latent double free, found via a fork); the scanner logs each
  eTRV once (plus pairing window changes) instead of every advertisement.

Targeted tests on the fixed build (VERBOSE), all passed: a write followed by a restart 0.3 s later is
delivered after the boot (2/2); set points outside the range (3.0, 4.5, 29.0 = above this eTRV's own
maximum of 28, 35.0) are refused without a link (4/4); ten commands within a second give one link and
one write of the last value (2/2); a second command 150 ms after the first never lets the first reach
the eTRV (8/8); pause and schedule round trips (OFF -> HEAT, AUTO -> HEAT) change only the mode byte.
Measured device behaviour: OFF -> HEAT restores the manual set point, AUTO -> HEAT keeps the
schedule's set point (documented in the README). Regression suite (parallel, same, mixed, cold): 0
timeouts, 0 anomalies, no WiFi or API connection loss, radio idle 8 % while work was pending.

The API connection loss at 03:04 during the old-policy run was a network-wide event: Home
Assistant's history shows eight WiFi devices of the house unavailable within 40 s at that moment
(most likely the access point) and the controller back one second later without a reboot.

**Heap exhaustion during a WiFi disturbance (13:06, production build, soak test running).** Two
other WiFi devices of the house dropped out of Home Assistant at 13:05:57-13:06:20; the controller
stayed associated but stopped delivering to its API clients (Home Assistant, the soak tool with a
DEBUG log subscription and a link every few seconds, a heap recorder), fell off the network at
13:08 and came back at 13:12 without a reboot - with 20.7 kB of free heap and a largest free block
of 1.3 kB (normally 75-78 kB / 57 kB). As soon as the test tools' connections were closed the heap
was back at 78 kB / 57 kB: not a leak, but data held for stalled TCP connections (ESP-IDF defaults:
up to 32 dynamic WiFi TX buffers, 5.7 kB TCP send buffer per connection, 12 retransmissions before a
connection is given up; ESPHome's own API backlog is capped at 8 messages per connection). With only
Home Assistant connected (no log subscription) the traffic during such a stall is a few state
updates. Not changed (no way to reproduce a stall of the ESP's own WiFi link here): the WiFi / lwIP
buffer settings. Advice: do not keep a log viewer connected permanently over a weak WiFi link.

**Soak of the production build (d0019b5), 7 h 46 min** (13:17-21:03, read-only links to all four
eTRVs as fast as the component allows, a write pair every 30 min; heap sampled every 30 s): 1028
links out of 1747 connection attempts (59 %; ~500 times the production link rate), 0 anomalies (no
watchdog, request timeout, protocol error, link lost with requests in flight, handle invalidation or
late open result), 0 component errors, no reboot, no API connection loss. Free heap 78.3 -> 75.9 kB
with a trend of +71 B/h (no leak); short dips to 59-66 kB (Home Assistant reconnects, WiFi) always
came back; largest free block median 57 kB, minimum 31.7 kB. Radio idle while work was pending: 2 %.
