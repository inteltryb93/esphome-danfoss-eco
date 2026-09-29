Danfoss Eco (merged, ESPHome 2026.x / ESP-IDF)
==============================================

The ``danfoss_eco`` climate platform creates a climate device which can be used to control a
Danfoss Eco eTRV over Bluetooth Low Energy from an ESP32.

This is a **merge of the [`ryssel`](https://github.com/ryssel/esphome-danfoss-eco) and
[`ckoca`](https://github.com/ckoca/esphome-danfoss-eco) forks** of
[`dmitry-cherkas/esphome-danfoss-eco`](https://github.com/dmitry-cherkas/esphome-danfoss-eco),
updated to build on **ESPHome 2026.9.0** (also tested with 2026.10.0-dev) with the **ESP-IDF** framework. See
[`MERGE_NOTES.md`](MERGE_NOTES.md) for the detailed, per-file merge rationale.

This component supports:

- Switch between Manual (HEAT), Schedule (AUTO) and Pause (OFF, frost protection only) modes
- Set the target room temperature
- Show the current temperature, current action (Heating/Idle/Off) and remaining battery level
- Keep the eTRV clock set (needed for the weekly schedule and vacations) and clear the latched E10
  "invalid clock" error after a battery change, exactly like the Danfoss app
- Report every eTRV error code E1–E15 (`problems`, `problems_detail` and per-fault binary sensors)
- Expose everything else the eTRV reports over Bluetooth: device mode (manual / schedule / vacation /
  pause), min/max/frost/vacation temperatures and vacation period, configuration flags (child lock,
  valve installed, automatic summer time, ...), the weekly schedule, the device name, PIN protection
  and the Device Information Service (firmware, hardware, serial number, ...) — see
  [Bluetooth characteristics](#bluetooth-characteristics)
- Managing multiple eTRVs from a single ESP32

It uses the ESP32 BLE peripheral, so a ``ble_client`` configuration must be provided.

> **Framework note:** XXTEA encryption is now **vendored** into the component, so the old
> `libraries: - xxtea-iot-crypt@2.0.1` line is **no longer required**. Build with the esp-idf
> framework:
> ```yaml
> esp32:
>   board: esp32dev
>   framework:
>     type: esp-idf
> ```
> The component compiles warning-free with ESPHome 2026.9.0 and 2026.10.0-dev on the default
> ESP-IDF (5.5.5); the hardware tests (4 eTRVs, ESP32-C3) ran on ESPHome 2026.9.0 / ESP-IDF 5.5.5.

Onboarding your device
------------------------
You need the MAC address of your Danfoss Eco. Either:
1. Look it up in the Danfoss Eco mobile app: `Settings -> System Information -> MAC Address`, or
2. Use the `danfoss_eco_scanner` sensor and check the ESPHome logs:
```yaml
esphome:
  name: etrv2wifi-scanner

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:
  level: INFO

external_components:
  - source:
      type: local
      path: components

sensor:
  - platform: danfoss_eco_scanner
    id: scanner
```
Press the hardware button on the eTRV to speed up discovery. Sample output:
```
[I][danfoss_eco_scanner:027]: Found Danfoss eTRV, MAC: 00:04:2F:xx:yy:zz, Name: 0;0:04:2F:xx:yy:zz;eTRV
```

Once the MAC is known:
```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf

external_components:
  - source:
      type: local
      path: components

ble_client:
  - mac_address: 00:04:2f:xx:yy:zz
    id: room_eco

climate:
  - platform: danfoss_eco
    name: "My Room eTRV"
    ble_client_id: room_eco
    pin_code: "0000"
    secret_key: deadbeefcafebabedeadbeefcafebabe
    battery_level:
      name: "My Room eTRV Battery Level"
    temperature:
      name: "My Room eTRV Temperature"
    problems:
      name: "My Room eTRV Problems"
    problems_detail:
      name: "My Room eTRV Problems Detail"
    connection:
      name: "My Room eTRV Connection"
    time_id: ha_time   # optional, keeps the eTRV clock set (needed for AUTO / schedule mode)

time:
  - platform: homeassistant
    id: ha_time
```

### Obtaining the `secret_key`
Danfoss Eco uses encrypted communication relying on the `secret_key`, which can only be read right
after the hardware button is pressed. Watch the ESPHome logs:
```
[I][danfoss_eco:...]: [My Room eTRV] Short press Danfoss Eco hardware button NOW in order to allow reading the secret key
```
If the button is not pressed in time, the read fails — restart the ESP32 and retry. On success the
key is stored in flash, and logged so you can pin it in your config:
```
[I][danfoss_eco:...]: [My Room eTRV] Consider adding below line to your danfoss_eco config:
[I][danfoss_eco:...]: [My Room eTRV] secret_key: deadbeefcafebabedeadbeefcafebabe
```

Configuration options
------------------------

- **id** (*Optional*): Manually specify the ID used for code generation.
- **name** (**Required**, string): The name of the climate device.
- **ble_client_id** (**Required**): The ID of the BLE Client.
- **pin_code** (*Optional*, string): Device PIN code (if configured). Exactly 4 digits.
- **secret_key** (*Required*, string): Device encryption key, 16 bytes / 32 hex characters.
- **update_interval** (*Optional*, time, default `30min`): how often the eTRV is read. Every read is
  one short BLE link (~2.5 s). Commands from Home Assistant are delivered immediately, independent of
  this interval.
- **battery_level** (*Optional*): Remaining battery level sensor.
- **temperature** (*Optional*): Current temperature (°C) sensor.
- **problems** (*Optional*): Binary problem sensor, ON while the eTRV reports any error code
  (E1–E15, see [Error codes](#error-codes)).
- **problems_detail** (*Optional*): Text sensor listing the active error codes, e.g.
  `Valve Stuck | Invalid Time | Low Battery | Very Low Battery | Motor Error (E6)`.
- **connection** (*Optional*): Diagnostic binary sensor (`connectivity`). ON after every successful
  read of the eTRV; OFF when the eTRV has not been reachable for `retry_window` (the pending request
  is kept and retried, see below) or after repeated protocol errors (wrong `pin_code` / `secret_key`).
- **time_id** (*Optional*, but strongly recommended; ID of a `time:` component, e.g.
  `platform: homeassistant`): keeps the eTRV clock set. The clock is read on every poll and written
  (at most once per hour) when it is off by more than 2 minutes, exactly in the format the Danfoss
  app uses: UTC time plus the *standard-time* offset. The eTRV adds the summer-time hour itself when
  its "automatic summer time" flag is on (see `daylight_saving`; with the flag off it runs on
  standard time all year, as with the app). The eTRV loses its clock
  on every battery change and then raises **E10 "invalid clock information"**; the flag is latched
  (the eTRV never clears it), so once the clock is right the component acknowledges it the way the
  app does (writes the error flags back without E10, at most once per hour). **Without `time_id` the
  `problems` sensor stays ON after every battery change, the weekly schedule does not run, and the
  eTRV falls back from AUTO (schedule) to manual mode by itself within minutes** (observed on
  hardware).
- **request_timeout** (*Optional*, time, default `15s`, range 2–60 s): if the eTRV does not answer a
  GATT request within this time the BLE link is torn down and the operation is retried on a new link.
  (The first request of a link - the PIN - is allowed at least 30 s.)
- **retry_window** (*Optional*, time, default `10min`, range 30 s–60 min): length of the fast retry
  phase (back-off 3 s, 15 s, 30 s, 60 s, 120 s). If the eTRV is still not reachable, the request is
  **kept** and retried every 5 minutes (every 15 minutes after one more hour) for up to 24 hours.
- **cache_services** (*Optional*, boolean, default `true`): keep the eTRV's GATT service table in
  flash (Bluedroid's NVS service cache, the same setting `bluetooth_proxy` uses). Without it ESPHome
  clears the table after every disconnect and the Bluetooth stack rediscovers all services at the start
  of every link, before the first request goes out. Measured on the four eTRVs: time from link open to
  the first answer 2.1 s → 0.19 s (median), 7.9 s → 0.21 s (p90); whole link 2.6 s → 0.7 s (median),
  10.7 s → 1.0 s (p90). The table is rebuilt from the eTRV whenever its layout looks different
  (unexpected length or ATT error) and before onboarding.
- **visual** (*Optional*): Standard ESPHome climate `visual:` block (min/max temperature, step).
  The component also auto-updates the displayed range from the eTRV's reported settings.

Optional entities (standard ESPHome sensor / binary sensor / text sensor options, e.g. `name:`).
All of them are diagnostic entities except `device_mode`. They cost no extra Bluetooth traffic,
except the ones marked *(information)*: those are read once after boot (the schedule again once a
day), only when configured, on a link that has finished its real work.

| Key | Type | Source | Meaning |
|---|---|---|---|
| `low_battery` | binary (battery) | error flags | E14 low battery or E15 critical battery |
| `valve_error` | binary (problem) | error flags | E9 valve cannot close |
| `motor_error` | binary (problem) | error flags | E6 motor jammed |
| `clock_error` | binary (problem) | error flags, clock | E10 invalid clock (battery changed), or a clock the eTRV does not keep (set, then found lost again — it raises no error code for that) |
| `hardware_error` | binary (problem) | error flags | E1–E5, E7, E8, E11–E13 (sensor, memory, radio, encoder …) |
| `device_mode` | text | settings | `manual`, `schedule`, `vacation`, `pause` |
| `temperature_min` / `temperature_max` | sensor °C | settings | allowed set point range |
| `frost_protection_temperature` | sensor °C | settings | temperature held in pause mode |
| `vacation_temperature` | sensor °C | settings | temperature held during a vacation |
| `vacation_start` / `vacation_end` | sensor (timestamp) | settings | planned vacation (unknown if none) |
| `child_lock` | binary | settings | buttons on the eTRV locked |
| `valve_installed` | binary | settings | installed and regulating (OFF = mounting mode) |
| `daylight_saving` | binary | settings | the eTRV switches to summer time by itself |
| `adaptive_learning` | binary | settings | "Forecast" (pre-heating) |
| `slow_regulation` | binary | settings | heat control "moderate" (OFF = "quick") |
| `vertical_installation` | binary | settings | thermostat mounted vertically |
| `display_flip` | binary | settings | display rotated |
| `pin_protection` | binary | *(information)* | a PIN is set on the eTRV (the PIN itself is never read out) |
| `thermostat_name` | text | *(information)* | name given in the Danfoss app |
| `schedule` | text | *(information)* | weekly "at home" periods, e.g. `Mo-Fr 06:00-08:00,16:00-22:00; Sa-Su 07:00-23:00` |
| `schedule_home_temperature` / `schedule_away_temperature` | sensor °C | *(information)* | schedule temperatures |
| `manufacturer`, `model`, `serial_number`, `hardware_version`, `firmware_version`, `software_version` | text | *(information)* | Device Information Service |

> **NOTE:** A minimal validation config is in `test_merged.yaml`; `sterownik-grzejnika.yaml` is a
> complete production configuration for four eTRVs.

Modes
-----

The eTRV mode byte (as the Danfoss app encodes it) is mapped to the Home Assistant climate modes:

| eTRV mode | raw | Climate mode | `device_mode` |
|---|---|---|---|
| manual | 0 | HEAT | `manual` |
| schedule | 1 | AUTO | `schedule` |
| vacation, then manual / schedule | 2 / 3 | AUTO | `vacation` |
| pause (frost protection only), then manual / schedule | 4 / 5 | OFF | `pause` |

Selecting HEAT writes 0, AUTO writes 1 and OFF writes a pause (5 if the eTRV was running its
schedule, else 4 — like the app). Selecting the mode Home Assistant already shows writes nothing, so
a running vacation or pause is never overwritten by accident. Like the app, the component never
writes a set point while the eTRV is paused (switch to HEAT first). Vacations are planned in the
Danfoss app; the component shows them but does not create them.

What the eTRV does with the set point when the mode changes (measured): leaving a pause (OFF ->
HEAT) brings back the manual set point it had before the pause; leaving the schedule (AUTO -> HEAT)
keeps the set point the schedule had at that moment (e.g. 17 C, not the manual value from before).
An automation that switches AUTO -> HEAT should therefore also send the set point it wants.

Error codes
-----------

The error characteristic holds 16 flags; flag *n* is error code E*n+1*. Titles as in the Danfoss app:

| Code | Meaning | Entity |
|---|---|---|
| E1, E2 | temperature sensor error | `hardware_error` |
| E3, E4, E5 | memory / hardware error | `hardware_error` |
| E6 | motor jammed (reset and re-mount) | `motor_error` |
| E7, E8 | communication module / invalid communication | `hardware_error` |
| E9 | valve cannot close (check the installation) | `valve_error` |
| E10 | invalid clock information — batteries replaced, time lost | `clock_error` (cleared automatically with `time_id`) |
| E11, E12, E13 | error E11 / radio communication error / encoder jammed | `hardware_error` |
| E14 | low battery | `low_battery` |
| E15 | critical battery (valve opened for frost protection) | `low_battery` |

All of them also switch `problems` on and are listed in `problems_detail`. So does a clock that the
eTRV loses again after it was set (`Clock Not Kept`, needs `time_id`): the eTRV raises no error code
for that, but its schedule and vacations cannot work.

Bluetooth characteristics
-------------------------

Complete list of the eTRV's GATT services and characteristics (from the official Danfoss Eco app
v1.5.0 and [BenjaminSoelberg/danfoss-eco2](https://github.com/BenjaminSoelberg/danfoss-eco2);
handles as found on the units in the field) and how this component handles each of them. The
custom characteristics are XXTEA-encrypted with the `secret_key`, except the PIN and the key itself.

| UUID | Handle | Access | Name | Handling |
|---|---|---|---|---|
| `10020001-…` | 0x24 | write | PIN | written first on every link |
| `10020002-…` | 0x27 | read/write | PIN settings ("thermostat code") | read once after boot → `pin_protection`; the PIN is wiped right after decryption, never written |
| `10020003-…` | 0x2a | read/write | settings (16 bytes) | read on every link → climate mode, `device_mode`, min/max/frost/vacation, configuration flags; **only the mode byte is written** |
| `10020004-…` | – | – | undefined | not present on these units, not used by the app |
| `10020005-…` | 0x2d | read/write | temperature (set point, room) | read on every link, set point written |
| `10020006-…` | 0x30 | read/write | device name | read once after boot → `thermostat_name`, never written |
| `10020007-…` | 0x33 | write | bootloader | **never touched** (a write reboots the eTRV into firmware-update mode) |
| `10020008-…` | 0x36 | read/write | clock (UTC + offset) | read on every link when `time_id` is set, written when off |
| `10020009-…` | 0x39 | read/write | error flags | read on every link → error entities; written only to clear E10 once the clock is right |
| `1002000A-…` | 0x3c | write | app language | not used (the app writes its UI language) |
| `1002000B-…` | 0x3f | read | secret key | only present during the pairing window; read once when no `secret_key` is configured |
| `1002000C-…` | 0x42 | read | firmware update token | not used (Danfoss cloud firmware download) |
| `1002000D/E/F-…` | 0x45/0x48/0x4b | read/write | weekly schedule (20 + 12 + 12 bytes) | read once after boot and once a day → `schedule`, `schedule_*_temperature`; never written |
| `10020010/11/12-…` | – | – | undefined | not present on these units, not used by the app |
| `0x2A19` (Battery) | 0x10 | read | battery level | read on every link → `battery_level` |
| `0x180A` (Device Information) | 0x13–0x21 | read | manufacturer, model, serial, hardware/firmware/software revision | read once after boot (firmware always, for the logs) → information text sensors |
| `0x1800` / `0x1801` | 0x01–0x0d | – | GAP / GATT | handled by the Bluetooth stack |
| `10010000-…` service | 0x4d–0x50 | notify | unknown (one characteristic) | not used by the app, never touched |
| `00060000-F8CE-11E4-ABF4-0002A5D5C51B` | – | – | Cypress OTA bootloader | only exists while the eTRV is in firmware-update mode, never touched |

All custom UUIDs share the suffix `-2749-0001-0000-00805F9B042F`.

BLE connection lifecycle (battery safety)
------------------------------------------

The eTRV is a battery device, so the component keeps every BLE link as short as possible and
guarantees that no link is ever left open:

1. `update()` (every `update_interval`) and Home Assistant commands only mark *pending operations*
   (read state / write set point / write mode / set clock). Nothing is queued as one-shot commands; a
   newer command simply replaces an older one that was not delivered yet (the last value wins).
2. A link is requested through the ESP32 BLE tracker's own "promotion" path (client state
   `DISCOVERED`) — the same mechanism `bluetooth_proxy` uses. The tracker stops the scan, raises the
   WiFi/BT coexistence preference to Bluetooth and serialises connection attempts. A direct
   `connect()` is only used as a fallback if the tracker does not promote the request within 15 s.
3. On the link: PIN → **read the device state** → pending writes, packed from the state read *in
   this link* → re-read → **disconnect and disable the `ble_client`**. A typical link lasts ~0.7 s.
   The component resolves the handles by service discovery on the first link after boot and reuses
   them afterwards (ESPHome `V3_WITH_CACHE` client mode); with `cache_services` the Bluetooth stack
   keeps the service table in flash, so it does not rediscover it on every link either (without the
   cache every link started with a ~2 s, at a weak signal >10 s, discovery by the stack). Anything
   unexpected at those handles (ATT error, wrong value length, rejected PIN) makes the next link
   discover again, from the eTRV itself - unless the handles already came from the eTRV itself (then
   it is a wrong `pin_code` or the eTRV's error code, and rediscovering every poll would only cost
   7-11 s of link time; such handles are trusted for a day).
4. If a link is lost half-way, the pending operations survive and are retried (see `retry_window`).
   A failed *open* (status 0x85, eTRV not heard for 20 s) costs the eTRV nothing, and waiting longer
   does not make the next attempt more likely to connect (measured: ~64 % of the attempts connect,
   whatever came before), so it is retried after 3 s, up to 6 times in a row. Meanwhile the client
   stays enabled (when the next link needs no service discovery), so the tracker's `auto_connect` can
   also connect the moment the eTRV is heard advertising. The tracker connects one client at a time;
   among thermostats that have failed, the one that has waited longest goes first (first come, first
   served). An eTRV that fails more often than that is most likely out of reach: it is parked between
   backed-off attempts (15 s ... 2 min, then the slow phase).
   ESPHome's BLE client logs every such attempt as
   `[E][esp32_ble_client] ESP_GATTC_OPEN_EVT in DISCONNECTING state (status=133)` followed by
   `[W] Connection open error, status=133` (the stack reports the failed connection before the open
   result). At a weak signal these lines are expected and harmless; the component's own
   `failed to open ... retry in N s` line that follows says what happens next.
5. Protocol errors that a retry cannot fix (rejected PIN, rejected request, data that does not
   decrypt to plausible values = wrong `secret_key`, missing characteristics) are counted; after 3 in
   a row the request is dropped until the next poll or command.
6. Safety nets, each of which alone prevents a hung link: the in-flight request counter is reset on
   every link loss, `request_timeout` tears down a link that stops answering, a 60 s link watchdog
   tears down any link older than that, and a client watchdog resets the ESPHome BLE client if it is
   ever stuck outside IDLE for 90 s without a link.

Home Assistant shows a requested set point / mode immediately; it is replaced by the value read back
from the eTRV after the write (or by the eTRV's real value if the request is dropped).

### Mode changes and the "valve reset" (upstream issue #11)

Upstream users reported that switching AUTO ↔ HEAT flipped the display, put the eTRV into "valve not
installed" (mounting) mode, locked the dial or set 55 °C. The cause was an upstream bug (v1.1.0–v1.1.4)
that encrypted and wrote only 8 of the 16 bytes of the settings characteristic, so the eTRV decrypted
random bytes. This component:

- writes all 16 bytes, byte-exact with an independent reference implementation (verified with
  27 000 test vectors covering all six modes), and **changes only the mode byte** — the other 15 bytes are sent back exactly as read from
  the eTRV in the same link (so nothing changed in the Danfoss app since the last poll is reverted);
- never stores or writes back data that does not decrypt to plausible values (protection against a
  wrong `secret_key`, which would otherwise reproduce the corruption);
- keeps mode and set point writes in separate links (conservative; not required once the 16-byte bug
  is fixed).

Verified on hardware: MANUAL → SCHEDULED → MANUAL on a real eTRV changes byte 4 only; configuration,
min/max, frost and vacation fields stay identical. Note that AUTO needs a valid eTRV clock (see
`time_id`).

YAML recommendations for the `ble_client` / tracker (all used in `sterownik-grzejnika.yaml`):

- keep the default `auto_connect: true` (the component enables the client only while it has work to
  do, so the tracker never opens unsolicited links; `auto_connect` is what makes recovery after a
  failed open fast),
- `esp32_ble: max_connections:` at least the number of eTRVs,
- `esp32_ble_tracker: scan_parameters: active: false` — passive scanning is all the component
  needs, and it spares the eTRVs a scan-response transmission for every advertisement the ESP hears,
- keep the scan running whenever WiFi is up: if you stop it while WiFi is disconnected (useful on
  the single-radio ESP32-C3), restart it in `wifi: on_connect` as the example configuration does —
  the component relies on the scan for `auto_connect` recovery,
- configure `time_id` (see above).

Testing tools
-------------

All tools need `aioesphomeapi`.

- `tools/ble_exercise.py` exercises the thermostats: every `--period` seconds it moves the set point
  of all thermostats in parallel (or one with `--single`) by `--delta` and back, optionally sends a
  second value right behind the first (`--double-tap`), reboots the device every `--restart-every`
  rounds, and verifies every command against the value **read back from the eTRV** (device log).
  It prints a summary (sent / verified / failed / superseded by other clients / latency).
- `tools/mode_test.py` switches one thermostat's mode (`--sequence off,heat`, `auto,heat`, ...) and
  compares the full settings block before and after.
- `tools/probe.py` prints every value the thermostats expose; with `--poke` it first triggers one BLE
  link per thermostat by re-sending the current set point (nothing is changed on the eTRV).
- `tools/capture_logs.py` records the complete device log; `tools/analyze_log2.py` summarises it per
  thermostat (links, discovery, PIN, reads/writes, close reason, warnings).
- `tools/press_restart.py` presses the restart button entity.
- `tools/scenario_test.py` times complete operations on a VERBOSE build: all thermostats at once,
  read-only links, back-to-back writes to one thermostat, A-B-A, a superseded command, mixed reads and
  writes, the first command after a reboot. Every command is followed through the device log from the
  API send to the value read back from the eTRV, with the phases in between; the report ends with the
  radio statistics (connection attempts, success rate, radio idle time while work was pending).
- `tools/soak_monitor.py` records heap, uptime, reconnects and the thermostat values as CSV over hours
  (leaks, unexpected reboots).

```
python tools/ble_exercise.py --host sterownik-grzejnika.local --prefix run --period 90 --delta 1.0 --double-tap 1.5 --restart-every 8
python tools/analyze_log2.py run.log
python tools/scenario_test.py --out scenarios.log --scenarios parallel,readonly,same,ab,double,mixed,cold --repeat 3
python tools/soak_monitor.py --out soak.csv --interval 60
```

See Also
--------

This component is based on the work of other authors:
* [AdamStrojek libetrv](https://github.com/AdamStrojek/libetrv) (with additional features from [spin83](https://github.com/spin83/libetrv) fork)
* MQTT bridge by [keton](https://github.com/keton/etrv2mqtt) and Home Assistant add-on by [HBDK](https://github.com/HBDK/Eco2-Tools)
* XXTEA implementation by [boseji](https://github.com/boseji) (vendored as `xxtea-lib.h` / `xxtea_core.*`)
