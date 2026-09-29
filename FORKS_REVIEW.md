# esphome-danfoss-eco: BLE connection-lifecycle survey of all forks, related projects and upstream issues

Date: 2026-09-27. Working dir for all clones: `forks/` (a scratch directory outside the repository) (`upstream/`, one dir per fork, `related/` for other projects, `api/` for raw GitHub API JSON, `diffs/` for local-vs-ryssel diffs).

Baseline compared against: `components/danfoss_eco` at commit `702c012` **plus** the uncommitted working-tree edits present at the time of writing (link diagnostics `log_link_()`, `gap_event_handler` for conn-param logging, and a 90 s `CONNECTION_WATCHDOG_MS` force-disconnect in `Device::loop()`).

Method: every fork was `git clone`d with all branches (`git fetch origin '+refs/heads/*:refs/remotes/origin/*'`), upstream was added as a remote, merge-base with `upstream/master` (`15b30a5`, 2023-11-18, the only upstream branch; tags v1.0.0..v1.1.4) computed per branch, and `git diff`/`git log -p` run against it. Upstream issues (29 items) and the comment threads of the connection-related ones were pulled via the unauthenticated API; 6 repo searches were run. Nothing under the repository root was modified.

---

## 1. Fork inventory

| Fork (repo) | Branches | Merge-base with upstream master | ahead / behind | Verdict |
|---|---|---|---|---|
| lrzio | master | 15b30a5 (= upstream HEAD) | 0 / 0 | **identical to upstream** |
| maryzhon | master | 15b30a5 | 0 / 0 | **identical** |
| cwannheden | master | 15b30a5 | 0 / 0 | **identical** |
| reinisb (esphome-danfoss-eco-mod) | master | f3de4ed | 0 / 22 | **identical** (its 2 commits 74196a2/f3de4ed were merged upstream as PR #16 in v1.1.3) |
| edwinrodenhuis (default `Adapting-espHome-v2022.10.2`) | Adapting-espHome-v2022.10.2, master | 4f61881 / 9dc418d | 0 / 18, 0 / 23 | **identical** (merged upstream as PR #15) |
| gerge | master | 349dd1f | 0 / 20 | **identical** (merged upstream as PR #13) |
| tryb103 | master | 24de47c (v1.1.0 era) | 0 / 28 | **identical**, stale |
| vzolotarev | master | 15b30a5 | 1 / 0 | trivial, no connection change (command.h one-liner, see 4.6) |
| Leworoe | master | 15b30a5 | 1 / 0 | trivial, no connection change (climate traits) |
| torbensp | master | c65262f (v1.1.2) | 3 / 24 | trivial (comments out `copy_address`), see 4.6 |
| stuartjbrown | master | 2049411 (v1.1.1) | 1 / 26 | trivial (climate.py device_class) |
| jaripetteri | master | 15b30a5 | 7 / 0 | xxtea vendoring + `test_xxtea()` self-test; **no connection change** |
| ckoca | master | 15b30a5 | 8 / 0 | substantive; already merged locally (4.2) |
| ryssel | master, fix-switching-between-heating-and-idle | 15b30a5 | 28 / 0, 20 / 0 | substantive; **4 BLE-hardening commits of 2026-07-21/22 are NOT in the local merge** (4.1) |
| inteltryb93 | master | 15b30a5 | 25 / 0 | ryssel's fix-switching branch + own `disconnect()` guard (4.3) |
| NicoJorgensen1 | master | 15b30a5 | 14 / 0 | version-gated `connect()`, `std::queue`, minute-slot polling, presets (4.4) |
| noesisaoliveira | master, main, before_gem | 15b30a5 | 63 / 64 / 47 | master ≈ ryssel fix-switching; main ≈ ckoca; before_gem = AI rewrite with no connect/disconnect at all (4.5) |

---

## 2. Upstream (dmitry-cherkas) connection-lifecycle history

Relevant commits on `master` (only branch):

- `810328d` 2021-11-21 "extract methods. connect once to read device name, then error 133" (first sighting of status 133/0x85).
- `2182741` 2021-11-23 **"fix connection failure (disable scan before connect), force connection instead of waiting for ble_tracker to discover the device (unfortunately it's not possible to disable the scan completely)"** – introduced the `remote_bda` pre-fill ("pretend, we have already discovered the device") and the `esp_ble_gap_stop_scanning()` call before connect.
- `a7c1b56` 2021-11-24 "refactor Device class to use commands queue and component loop() to manage device communication" – introduced `CommandQueue`, `request_counter_`, disconnect-when-counter-hits-0.
- `8009554`/`4d7ff33` 2021-11-28 "halt and disconnect, if failed to write PIN" / "disconnect, if failed to write pin" (→ `mark_failed()`).
- `39efc70` 2021-11-28 "trigger update() on write, so that latest device state is reported back to HA" (the `on_write()` → `update()` re-read).
- `c40a7af` 2023-11-18 (ckoca, PR #19, tag v1.1.4) "Fix bluetooth connection issues with ESPHome 2023.11" – full diff:

```diff
@@ Device::setup()
       copy_address(this->parent()->get_address(), this->parent()->get_remote_bda());
+      this->parent()->set_state(ClientState::INIT);
@@ Device::connect()
-      if (this->node_state == ClientState::ESTABLISHED)
+      if (this->node_state == ClientState::INIT || this->node_state == ClientState::ESTABLISHED)
       {
         return;
       }
@@
       esp_ble_gap_stop_scanning();
-      this->parent()->set_state(ClientState::DISCOVERED); // this will cause ble_client to attempt connect() from its loop()
+      this->parent()->set_state(ClientState::READY_TO_CONNECT); // this will cause ble_client to attempt connect() from its loop()
```

Upstream author statements about the intended lifecycle (issues):

- #4 (dmitry-cherkas, 2022-01-30): "**This component does not keep the BLE connection open, it only checks on the device from time to time**"; offers to "set warning status, when connection to the device failed and clear it on the next successful connection". Reporter bipsendk: TCP/API connection is lost while the ESP queries BLE, and "I sometime also see BLE connection error 133".
- #19 (dmitry-cherkas, 2023-11-18) answering ryssel's log ending in `Disabling BLE client.`: "`Disabling BLE client.` is not an issue actually. **ESP32 does not maintain open connection with the Eco, it polls for status updates from time to time, establishing a new connection and shutting down the BLE client once done.**"

### 2.1 Issues / PRs relevant to hanging connections, reconnects, battery

| # | Title | Key facts |
|---|---|---|
| #18 (closed) | After updating to ESPHome 2023.11.0 cannot establish bluetooth connection | Log loops `requesting device state` / `[esp32_ble_client] 0x00 Attempting BLE connection` every minute with no OPEN. Cause: ESPHome PR esphome/esphome#5704 changed the client state machine. Fixed by #19 (`set_state(INIT)` in setup, `READY_TO_CONNECT` instead of `DISCOVERED`). |
| #19 (PR, merged v1.1.4) | Fix bluetooth connection issues with ESPHome 2023.11 | diff above. ryssel's confirming log shows the healthy cycle: `Found device` → `Pausing scan to make connection...` → `Attempting BLE connection` → 6 s → `Connected` → `Starting scan...` → 4 reads → `Disabling BLE client.` |
| #25 (open) | Compilation after ESPHome 2025.7.0 | `esp32_ble_tracker::Queue<Command>` removed; then (2025.10.5) `ClientState::READY_TO_CONNECT` removed ("probably the right entry now would be simply DISCOVERED"). ryssel fork and ckoca #29 fix it. ryssel notes the new build "uses more memory" (needed `min_spiffs.csv`). |
| #29 (PR, open) | ESP-IDF Support, Problems-detail Sensor, ESPHome 2025.11.0 Compatibility (ckoca) | The ckoca fork; reinisb: "successfully switched to your fork and the IDF version is working great"; bassguitarist confirms 2026.1 build. |
| #24 (open) | esp-idf framework | xxtea-iot-crypt is Arduino-only → ckoca vendored XXTEA; no connection-behaviour differences reported between frameworks. |
| #11 (closed, 38 comments) | Unable to compile with 2022.9.4 ... compiles but does not run stable | hcjehg: **"OK, sometimes the above does not get to disconnect the ble..."**; "When the thermostat tilts I need to go back in with the Danfoss app ... and while doing that I need to kill my esphome, or it will try to connect all the time". marcin7d: with 2–3 valves "the system goes crazy ... the connection freezes and the thermostats glow red". FransOv: **"the bluetooth connection is critical. Keep distance to 5 meters or less and free of obstructions ... Even then it can take several minutes to establish communication"**; switching mode auto<>heat "resets the valve"; "You can set the temperature either manually or via Home Assistant, only not while the esp is communicating with the valve." reinisb: after key read "it is not possible to set temperature on the physical Eco thermostat any longer, and frequently it would suddenly start heating to 60 C" (dmitry: "intermittently it looses the internal state and writes garbage to the Eco" – the 8-vs-16-byte settings bug later fixed by ryssel). |
| #5 (open) | Using NimBLE for >3 devices | ble_client limited to 3 devices; users deploy multiple ESP32s. |
| #22 (open) | Step by step guide | Masterz69: secret-key read only succeeded once the separate `danfoss_eco_scanner` component was removed from the same ESP (scanner and client interfere). |
| #20 (PR) | Fix uninitialized target temperature | not connection related (fixed upstream in ESPHome 2023.12). |
| #23 | Battery % always wrong | NiMH cells read ~25–36 %; not connection related. |
| #27, #26, #28 | 2025.x compile errors | Queue / CLIMATE_SCHEMA / READY_TO_CONNECT removals (same as #25). |

No issue mentions battery drain caused by the component; the only battery-drain statement in the ecosystem is etrv2mqtt's `stay_connected` option doc (section 5.2).

---

## 3. Detailed per-fork findings

### 3.1 ryssel/esphome-danfoss-eco (the important one)

Branches: `fix-switching-between-heating-and-idle` (20 commits, tip `554d023` 2026-01-21) and `master` (28 commits, tip `3e7bcf9` 2026-07-22). The local merge (`5a19320`, 2026-06-30) predates the last 8 master commits:

```
3e7bcf9 2026-07-22 Document auto_connect guidance for Danfoss BLE stability   (README only)
421fd64 2026-07-22 Add per-device cooldown after BLE open failures
8378c00 2026-07-21 fix: serialize BLE connect attempts across Danfoss devices
277ad11 2026-07-21 Improve BLE connection robustness and timeout handling
e8491af 2026-04-07 Update paths for new workspace location (no spaces)         (DEVELOPMENT.md)
c45143f 2026-04-02 Fix deprecated ESPBTUUID::to_string() calls                (in local)
ec3b21f 2026-04-02 Add development setup guide and IntelliSense build config  (docs)
f71b9ef 2026-04-02 Merge branch 'fix-switching-between-heating-and-idle'
```

Earlier ryssel commits that ARE in the local copy (for the record): `49bf77d` 2025-10-15 `set_state(READY_TO_CONNECT)` → `parent()->connect()`; `7e1e273` 2025-10-15 removed `set_state(ClientState::INIT)` from setup() and the `INIT ||` guard from connect() ("Manual state management via set_state() - not recommended in modern ESPHome"); `2cd31a1` 2025-10-15 **"Removed esp_ble_gap_stop_scanning() - this was preventing the second device from connecting"**; `ef1f23a`/`ccdfbfa` 2025-07-19 safety guards in control() (mode+temperature never in one cycle, null-data checks, 5–30 °C range, write-on-change) — commit message: "when switching between heat and auto modes, the code was modifying the SettingsData structure and writing all 16 bytes back to the device. If any part of that data was corrupted or uninitialized, it would crash the thermostat's firmware, requiring battery removal"; `7086299`/`f469193` the 8-vs-16-byte settings encryption fix; `b723b31` 2025-07-20 removed the debugging/vacation-date defensive code.

#### 277ad11 "Improve BLE connection robustness and timeout handling" (2026-07-21) — verbatim device.cpp/device.h/command.h/climate.py changes

climate.py: new option
```python
CONF_REQUEST_TIMEOUT = 'request_timeout'
...
            cv.Optional(CONF_REQUEST_TIMEOUT, default="15s"): cv.positive_time_period_milliseconds,
...
    cg.add(var.set_request_timeout_ms(config[CONF_REQUEST_TIMEOUT].total_milliseconds))
```
command.h (ryssel uses a FreeRTOS `xQueueCreate(32, sizeof(Command*))` queue, `push()` with `portMAX_DELAY`): adds
```cpp
            size_t size() const { return uxQueueMessagesWaiting(queue_handle_); }
            void clear() { Command *cmd = nullptr; while (xQueueReceive(queue_handle_, &cmd, 0) == pdTRUE) { delete cmd; } }
```
device.h additions:
```cpp
      void set_request_timeout_ms(uint32_t timeout_ms);
      void request_device_state_();
    private:
      void teardown_connection_(bool clear_queue, bool reset_backoff, const char *reason);
      uint8_t request_counter_ = 0;
      bool request_watchdog_active_ = false;
      uint32_t request_watchdog_started_ms_ = 0;
      uint32_t request_watchdog_timeout_ms_ = 15000;
      uint32_t request_timeout_ms_ = 15000;
      uint8_t timeout_backoff_level_ = 0;
      bool preserve_backoff_on_next_disconnect_ = false;
      bool scheduled_poll_pending_ = false;
      uint32_t scheduled_poll_due_ms_ = 0;
      uint16_t poll_spread_ms_ = 0;
      static constexpr uint32_t REQUEST_TIMEOUT_MIN_MS = 1000;
      static constexpr uint32_t REQUEST_TIMEOUT_MAX_MS = 60000;
```
device.cpp:
```diff
 #include "device.h"
+#include <algorithm>
 #include <cmath>
+#include <esp_timer.h>
@@
+    static bool startup_marker_logged = false;
+    static uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }
+
     void Device::setup()
     {
+      if (!startup_marker_logged) { startup_marker_logged = true;
+        ESP_LOGI(TAG, "[BLE_FLOW][BUILD_MARKER] danfoss_eco_ble_hardening_v3 compiled %s %s", __DATE__, __TIME__); }
@@
       copy_address(this->parent()->get_address(), this->parent()->get_remote_bda());
+      const uint8_t *bda = this->parent()->get_remote_bda();
+      const uint16_t seed = (static_cast<uint16_t>(bda[4]) << 8) | bda[5];
+      this->poll_spread_ms_ = 200 + (seed % 3000);      // per-device 200..3199 ms poll offset derived from MAC
     }

     void Device::loop()
     {
       if (this->status_has_error())
       {
-        this->disconnect();
+        this->teardown_connection_(true, true, "status error");
         this->status_clear_error();
       }
+
+      if (this->scheduled_poll_pending_ && static_cast<int32_t>(now_ms() - this->scheduled_poll_due_ms_) >= 0)
+      {
+        this->scheduled_poll_pending_ = false;
+        this->connect();
+        this->request_device_state_();
+      }
+
       if (this->node_state != ClientState::ESTABLISHED)
+      {
+        if (this->node_state == ClientState::IDLE && !this->commands_.empty())
+          this->connect();                              // reconnect while work is queued
         return;
+      }

       Command *cmd = this->commands_.pop();
       while (cmd != nullptr)
       {
-        if (cmd->execute(this->parent()))
+        const bool request_sent = cmd->execute(this->parent());
+        if (request_sent)
+        {
+          if (this->request_counter_ == 0xFF)
+          {
+            ESP_LOGW(TAG, "[%s] request counter overflow guard hit, forcing disconnect", ...);
+            delete cmd;
+            this->teardown_connection_(true, true, "request counter overflow");
+            return;
+          }
           this->request_counter_++;
+          if (!this->request_watchdog_active_)
+          {
+            this->request_watchdog_active_ = true;
+            this->request_watchdog_started_ms_ = now_ms();
+            const uint32_t timeout_factor = 1U << this->timeout_backoff_level_;
+            this->request_watchdog_timeout_ms_ = std::min(this->request_timeout_ms_ * timeout_factor, REQUEST_TIMEOUT_MAX_MS);
+          }
+        }
         delete cmd;
         cmd = this->commands_.pop();
       }

+      if (this->request_watchdog_active_ && this->request_counter_ > 0)
+      {
+        const uint32_t elapsed_ms = now_ms() - this->request_watchdog_started_ms_;
+        if (elapsed_ms >= this->request_watchdog_timeout_ms_)
+        {
+          ESP_LOGW(TAG, "[%s][BLE_FLOW] request watchdog timeout: elapsed=%u ms, pending=%u - forcing disconnect", ...);
+          if (this->timeout_backoff_level_ < 5)
+            this->timeout_backoff_level_++;
+          this->preserve_backoff_on_next_disconnect_ = true;
+          this->teardown_connection_(true, false, "request watchdog timeout");
+          return;
+        }
+      }
       if (this->request_counter_ == 0)
         this->disconnect();
     }

     void Device::update()
     {
-      this->connect();
-      if (this->xxtea->status() == XXTEA_STATUS_SUCCESS) { ... push 4 READ commands ... }
+      if (!this->scheduled_poll_pending_)
+      {
+        this->scheduled_poll_pending_ = true;
+        this->scheduled_poll_due_ms_ = now_ms() + this->poll_spread_ms_;
+      }
     }
@@ gattc_event_handler
       case ESP_GATTC_DISCONNECT_EVT:
-        ESP_LOGD(... "disconnect, conn_id=%d, reason=%#04x" ...);
+      {
+        const bool reset_backoff = !this->preserve_backoff_on_next_disconnect_;
+        this->preserve_backoff_on_next_disconnect_ = false;
+        this->teardown_connection_(false, reset_backoff, "gatt disconnect event");   // NEW: DISCONNECT_EVT now resets counter/state
+      }
         break;
@@
     void Device::on_read(...)
     {
+      if (this->request_counter_ == 0) { ESP_LOGW(... "read response with empty pending counter" ...); return; }
       this->request_counter_--;
+      if (this->request_counter_ == 0) { this->request_watchdog_active_ = false; this->request_watchdog_timeout_ms_ = this->request_timeout_ms_; this->timeout_backoff_level_ = 0; }
       ...
     void Device::on_write(...)
     {
+      if (this->request_counter_ == 0) { ESP_LOGW(... "write response with empty pending counter" ...); return; }
       this->request_counter_--;
+      (same watchdog reset)
       if (param.status != ESP_GATT_OK) ESP_LOGW(...);
       else
-        update();
+        this->request_device_state_();     // re-read without re-arming the poll scheduler

+    void Device::request_device_state_()
+    {
+      if (this->xxtea->status() != XXTEA_STATUS_SUCCESS) return;
+      ESP_LOGI(TAG, "[%s] requesting device state", ...);
+      push READ battery, temperature, settings, errors
+    }
@@
     void Device::disconnect()
     {
-      this->parent()->set_enabled(false);
-      this->node_state = ClientState::IDLE;
+      this->teardown_connection_(false, true, "normal disconnect");
     }
+
+    void Device::set_request_timeout_ms(uint32_t timeout_ms)
+    {
+      this->request_timeout_ms_ = std::max(REQUEST_TIMEOUT_MIN_MS, std::min(timeout_ms, REQUEST_TIMEOUT_MAX_MS));
+      this->request_watchdog_timeout_ms_ = this->request_timeout_ms_;
+    }
+
+    void Device::teardown_connection_(bool clear_queue, bool reset_backoff, const char *reason)
+    {
+      if (this->request_counter_ > 0 || !this->commands_.empty())
+        ESP_LOGW(TAG, "[%s][BLE_FLOW] teardown (%s): pending=%u, queued=%u, clear_queue=%d, reset_backoff=%d", ...);
+      this->parent()->set_enabled(false);
+      this->request_counter_ = 0;
+      this->request_watchdog_active_ = false;
+      if (reset_backoff) { this->request_watchdog_timeout_ms_ = this->request_timeout_ms_; this->timeout_backoff_level_ = 0; }
+      if (clear_queue) this->commands_.clear();
+      this->node_state = ClientState::IDLE;
+    }
```

#### 8378c00 "fix: serialize BLE connect attempts across Danfoss devices" (2026-07-21)
Commit message: "Add a global connect slot with cooldown and per-device connect throttling to avoid concurrent connection races. Also bump build marker to ble_hardening_v4".
```diff
+    static Device *connect_slot_owner = nullptr;
+    static uint32_t connect_slot_available_at_ms = 0;
@@ device.h
+      uint32_t last_connect_attempt_ms_ = 0;
+      static constexpr uint32_t CONNECT_SLOT_COOLDOWN_MS = 700;
+      static constexpr uint32_t CONNECT_ATTEMPT_INTERVAL_MS = 1000;
@@ Device::connect()
+      const uint32_t now = now_ms();
       if (this->node_state == ClientState::ESTABLISHED) return;
+      if (static_cast<int32_t>(now - this->last_connect_attempt_ms_) < static_cast<int32_t>(CONNECT_ATTEMPT_INTERVAL_MS))
+        return;                                                   // at most one attempt per second per device
+      if (connect_slot_owner != nullptr && connect_slot_owner != this)
+      { this->last_connect_attempt_ms_ = now; ESP_LOGV(... "connect slot busy, waiting"); return; }
+      if (connect_slot_owner == nullptr)
+      {
+        if (static_cast<int32_t>(now - connect_slot_available_at_ms) < 0)
+        { this->last_connect_attempt_ms_ = now; ESP_LOGV(... "connect slot cooling down"); return; }
+        connect_slot_owner = this;
+        ESP_LOGD(TAG, "[%s][BLE_FLOW] acquired connect slot", ...);
+      }
+      this->last_connect_attempt_ms_ = now;
       ... (re-enable ble_client, parent()->connect())
@@ Device::teardown_connection_()
+      if (connect_slot_owner == this)
+      {
+        connect_slot_owner = nullptr;
+        connect_slot_available_at_ms = now_ms() + CONNECT_SLOT_COOLDOWN_MS;   // 700 ms gap before the next device may connect
+      }
       this->parent()->set_enabled(false);
```

#### 421fd64 "Add per-device cooldown after BLE open failures" (2026-07-22)
```diff
@@ device.h
+      uint32_t open_fail_cooldown_until_ms_ = 0;
+      static constexpr uint32_t OPEN_FAIL_RETRY_COOLDOWN_MS = 5000;
@@ ESP_GATTC_OPEN_EVT
         if (param->open.status == ESP_GATT_OK)
           ESP_LOGV(... "open" ...);
         else
+        {
+          this->open_fail_cooldown_until_ms_ = now_ms() + OPEN_FAIL_RETRY_COOLDOWN_MS;
           ESP_LOGW(TAG, "[%s] failed to open, conn_id=%d, status=%#04x", ...);
+          ESP_LOGW(TAG, "[%s][BLE_FLOW] applying open-fail cooldown: %u ms", ...);
+        }
@@ Device::connect() (after the ESTABLISHED early-return)
+      if (static_cast<int32_t>(now - this->open_fail_cooldown_until_ms_) < 0)
+      {
+        (log once per CONNECT_ATTEMPT_INTERVAL_MS: "open-fail cooldown active: %u ms remaining")
+        return;
+      }
```
Note: ryssel does NOT disable the client on a failed OPEN (same conclusion as the local revert `702c012`); it only throttles the next `connect()` call for 5 s.

#### 3e7bcf9 README additions (2026-07-22) — verbatim
```
- **request_timeout** (**Optional**, duration, default=`15s`): Maximum time to wait for in-flight BLE request responses before forcing disconnect and retry.

`request_timeout` tuning guidance:
- Use `10s` to `15s` in stable BLE environments for faster recovery from stalled requests.
- Use `20s` to `30s` in noisy BLE environments to reduce false watchdog timeouts.
- The component applies capped backoff after repeated timeouts and resets to the configured value after disconnect/recovery.

BLE connection stability behavior:
- Connection attempts are serialized across Danfoss devices (single global connect slot).
- After BLE open failures (for example status `133` / `0x85`), the affected device applies a per-device retry cooldown (currently 5 seconds) before trying again.
- This cooldown improves resilience even if `ble_client` is accidentally left with `auto_connect: true`.
- `auto_connect` is a generic `ble_client` option designed for convenience and automatic reconnect in always-on BLE integrations.
- For this component, especially with multiple Danfoss devices, `auto_connect: false` is recommended so the component can fully control connect/disconnect timing.
```
(README example config adds `request_timeout: 15s` under the climate entry. None of ryssel's example YAMLs actually set `auto_connect`.)

Other ryssel-master vs local differences (non-lifecycle): ryssel has no `problems_detail` text sensor, no `cv.sensitive`, uses `char buf[37]` instead of `UUID_STR_LEN`, FreeRTOS queue instead of `LockFreeQueue`, `%02x`-style raw-byte logging instead of `format_hex_pretty`. Full diffs: `diffs/local_vs_ryssel_master_*.diff`.

### 3.2 ckoca/esphome-danfoss-eco (8 commits, tip `ac72c9d` 2026-04-15) — PR #29

Connection-related content (all already reflected in the local merge except where noted):
```diff
@@ command.h
+#include "esphome/core/lock_free_queue.h"
-        class CommandQueue : public esphome::esp32_ble_tracker::Queue<Command>
+        class CommandQueue : public esphome::LockFreeQueue<Command, MAX_BLE_QUEUE_SIZE>
-            bool is_empty() { return this->q_.empty(); }
+            bool is_empty() { return this->empty(); }
@@ device.cpp Device::connect()
       // gap scanning interferes with connection attempts, which results in esp_gatt_status_t::ESP_GATT_ERROR (0x85)
       esp_ble_gap_stop_scanning();            <-- ckoca KEEPS the raw scan stop (local removed it, following ryssel 2cd31a1)
-      this->parent()->set_state(ClientState::READY_TO_CONNECT);
+      this->parent()->connect();
```
ckoca keeps `this->parent()->set_state(ClientState::INIT);` in setup() and the `INIT ||` guard in connect() (local removed both). Nothing else in ckoca touches connect/disconnect/retries. Commits: `38c7efc` esp-idf compat (vendored XXTEA), `19cb449` LockFreeQueue, `a5b4fdf` problems_detail, `c4ee70f` 2025.10.0 compat (connect()), `f3339ff` 2025.11.0 compat, `b30826f` min/max temperature config, `ac72c9d` to_str().

### 3.3 inteltryb93/esphome-danfoss-eco (25 commits, tip `2e09d8e` 2025-10-15; author trybson + cherry-picked ryssel)

Content = ryssel `fix-switching-between-heating-and-idle` merged in 2025-09-28 + ryssel's three 2025-10-15 commits cherry-picked (`09c20eb` left a conflict marker; `2e09d8e` "fix connect method" resolved it by dropping the `esp_ble_gap_stop_scanning()` line). Own connection-related commits:

- `1b0bcc1` 2024-05-27 trybson **"fix disconnect"**:
```diff
     void Device::disconnect()
     {
-      this->parent()->set_enabled(false);
+      if (this->parent()->enabled)
+      {
+        ESP_LOGD(TAG, "[%s] disabling ble_client", this->get_name().c_str());
+        this->parent()->set_enabled(false);
+      }
       this->node_state = ClientState::IDLE;
     }
```
  (Functionally a no-op vs upstream: `BLEClient::set_enabled()` already returns early when the flag is unchanged; it only removes the redundant call/log.)
- `43988ab` 2024-12-17 trybson "test": removed `esp_ble_gap_stop_scanning();` (independently of ryssel, 10 months earlier); `e99b205` "fix" touches only xxtea.
- `1e9bc5b` 2024-11-08 "fix pin format" (`%04d` → `%04ld` for the PIN log).
- `4c364c7` 2024-05-27 "fix esp-idf framework crashes during compilation" (xxtea only).

Diff of inteltryb93 master vs ryssel `554d023`: only the `disconnect()` guard above, `%04ld`, `address_str().c_str()`, and an older `my_component.h` (HEAT-only traits, visual 6–28, no `set_temperature_range`). Nothing further on the lifecycle.

### 3.4 NicoJorgensen1/esphome-danfoss-eco (14 commits, 2025-09-08..2025-11-05)

- `1cd470f` 2025-11-04 "Enhance Bluetooth connection handling for ESPHome 2025.10.0 and later":
```diff
 #ifdef USE_ESP32
+#include "esphome/core/version.h"
@@ Device::connect()
       // gap scanning interferes with connection attempts, which results in esp_gatt_status_t::ESP_GATT_ERROR (0x85)
       esp_ble_gap_stop_scanning();
+      #if ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 10, 0)
+      this->parent()->connect();  // request a connection on new ESPHome
+      #else
       this->parent()->set_state(ClientState::READY_TO_CONNECT); // this will cause ble_client to attempt connect() from its loop()
+      #endif
```
  Keeps `set_state(ClientState::INIT)` in setup() and the `INIT ||` guard.
- `d2221b4` 2025-09-08: `CommandQueue` reimplemented on `std::queue<Command *>` (push/pop/is_empty).
- `eb22260` 2025-11-04 "multi-thermostat support": time-slot polling to keep several eTRVs on one ESP32 from polling simultaneously —
```cpp
    void Device::update()
    {
      // Check if it's time for this thermostat to update based on minute-based scheduling
      if (this->max_thermostats_ > 1)
      {
        time_t now = time(nullptr);
        struct tm *timeinfo = localtime(&now);
        int current_minute = timeinfo->tm_min;
        // Calculate interval: 60 minutes / max_thermostats (e.g., 60/5 = 12 minutes)
        int interval_minutes = 60 / this->max_thermostats_;
        //   Slot 0: updates at :00, :12, :24, :36, :48 (minute % 12 == 0)
        //   Slot 1: updates at :01, :13, :25, :37, :49 (minute % 12 == 1)
        if ((current_minute % interval_minutes) != this->update_slot_)
        {
          ESP_LOGD(TAG, "[%s] skipping update - not this thermostat's slot ...");
          return;
        }
      }
      this->connect();
      ...
```
  climate.py: `update_slot: cv.int_range(0,4)`, `max_thermostats: default 5`. Their `yaml_config_in_home_assistant.yml` uses `esp32_ble: max_connections: 5`, `esp32_ble_tracker: scan_parameters: active: false`, `update_interval: 12min` per thermostat, arduino framework. (No `auto_connect` setting.)
- Rest: presets (AWAY = vacation) in control()/properties.cpp, `BUGFIX_PRESET_REVERT.md` (the post-write `update()` re-read overwrote the HA preset). No changes to disconnect/retry/request counting.

### 3.5 noesisaoliveira/esphome-danfoss-eco (3 branches, 2026-02-11..02-24, 60+ commits titled "update"/"test"/"gem vN")

- `master` (tip `887ae6a` "ryssel"): content identical to ryssel `fix-switching-between-heating-and-idle` (FreeRTOS queue, no `set_state(INIT)`, no `esp_ble_gap_stop_scanning()`, `parent()->connect()`, safety guards). No original lifecycle change.
- `main` (tip `d9b4377` "new"): content identical to ckoca (keeps `esp_ble_gap_stop_scanning()` and `set_state(ClientState::INIT)`; `parent()->connect()`; reverts ryssel's control() safety guards).
- `before_gem` (tip `f3244ae`): an AI-generated rewrite (`Device` is no longer a `BLEClientNode`; `MyComponent` owns a `Device`). Lifecycle-relevant properties of that rewrite: **no `connect()`/`disconnect()` at all** (relies entirely on `ble_client` auto-connect and never disables the client); `loop()` deletes every queued command whenever `node_state != ESTABLISHED`; `update()` queues only temperature+battery reads (no settings/errors); `write_pin()` returns without writing when `pin_code_ == 0` (i.e. the default PIN 0000 is never sent, which per every other project is required before reads work); no request counter. Not a usable reference.

### 3.6 Trivial forks (no lifecycle changes)

- vzolotarev `da0fcaf`: `class CommandQueue : public esphome::esp32_ble_tracker::Queue<Command>` → `...::Queue` (the fork of issue #27's author; does not compile on new ESPHome).
- Leworoe `6893496`: traits HEAT only, `set_visual_min_temperature(6)` / `max(28)`.
- torbensp `589c83e` (based on v1.1.2): `copy_address(...)` line commented out → the "pretend discovered" pre-fill is gone (would break forced connect on 2022.10 ESPHome).
- stuartjbrown `840c938`: removes `device_class` from the problems binary sensor schema.
- jaripetteri: vendored `xxtea_c.c/h`, `test_xxtea()` called at the end of `setup()`; climate.py schema modernised. No device.cpp lifecycle change.

### 3.7 Identical forks
lrzio, maryzhon, cwannheden, reinisb, edwinrodenhuis, gerge, tryb103 – nothing beyond upstream master (reinisb/edwinrodenhuis/gerge commits were merged upstream in v1.1.3).

---

## 4. Related projects — what they say/do about the eTRV connection lifecycle

### 4.1 AdamStrojek/libetrv (Python, bluepy; last commit 2024-11-13, v0.6) and spin83/libetrv
- Connection model: `eTRVDevice.connect(send_pin=True)` → `btle.Peripheral(address)`; on `BTLEDisconnectError` it **retries immediately with 100 ms sleep**, up to `retry_limit` (None = forever): "Unable connect to {}. Retrying in 100ms".
- PIN: written once per connection to **handle 0x24**, 4 bytes big-endian (`b'\0\0\0\0'` default), `writeCharacteristic(0x24, pin, True)`; `__pin_already_sent` is reset on disconnect. The secret key is read from the key characteristic only when the advertisement name's leading flag digit has bit 2 set (`flags & 0x4` = setup mode after the button press).
- Every property access auto-connects if needed (`etrv_read_data`: `if not device.is_connected(): device.connect(send_pin)`); **the library never disconnects by itself** — callers must call `disconnect()`, which also invalidates all cached fields.
- No statement about how long the eTRV keeps a link open or whether it drops it.
- spin83 fork: only `Scanner().scan(1)` instead of `scan(2)` and an import fix. No lifecycle change.

### 4.2 keton/etrv2mqtt (Python, on libetrv; README now says the base library "stopped updating") and HBDK/Eco2-Tools (HA add-on packaging of the same)
- Poll cycle (`devices.py`): `if not connected: connect(); read name/battery/temperature; if not stay_connected: disconnect()`. After `set_temperature` it immediately re-polls ("Home assistant needs to see updated temperature value to confirm change").
- Config docs (`docs/config_json.md`, identical in HBDK `DOCS.md`): `poll_interval` default **3600 s**; `retry_limit` "Limit of BLE connect attempts", default 5; **`stay_connected` — "Set to true in order to leave BLE connection running after polling thermostat data or setting temperature. May drain battery."** default false; `setpoint_debounce_time` 3 s.
- This is the only explicit battery-drain statement found; it is about holding the link open.

### 4.3 jarikp/ESP32_DanfossECO2 (ESP32 Arduino BLE library, 2020)
- Short-lived connection per read: `connect()` = `pClient->connect(addr, BLE_ADDR_TYPE_PUBLIC)` → write PIN (4 ASCII-digit bytes, e.g. `0,0,0,0`) to `10020001-...` → `refreshValues()` (name, temperature, battery). If the PIN write fails or values can't be decrypted it calls `disconnect()`.
- `disconnect()` calls `pClient->disconnect()` and **busy-waits `while (pClient->isConnected()) delay(100);`** before returning.
- Example sketch: `loop(){ if (eTRV->connect()) { print; eTRV->disconnect(); } delay(30000); }` — connect/read/disconnect every 30 s. `pair()` reads the key characteristic first, then does a normal connect.
- No retry/backoff logic; no statement about the device dropping links.

### 4.4 deltasystems-pl/danfoss-eco-ha (HA integration, bleak via HA bluetooth/ESPHome proxies; v1.3.0 2026-08-20) — most recent field knowledge
- `ble.py` header: "Measured on real Eco 2 units (2026-08): **connect + service discovery ~3 s, characteristic reads <1 s** - comfortably inside proxy GATT limits despite the device's slow-BLE reputation (which stems from BlueZ hosts)." README: "The Danfoss Eco has a reputation for being painfully slow over BLE (60–90 s operations). Our measurements show that is a BlueZ artifact: through an ESP32-based Bluetooth proxy the same device connects and completes a full state read in a few seconds. A close proxy (RSSI better than about −90 dBm) gives the best experience."
- `_BLE_LOCK = asyncio.Lock()` — "All eTRVs in a home usually share one Bluetooth proxy, and a proxy has very few connection slots. Letting three thermostats dial out at the same time is the fastest way to produce 'no backend with an available connection slot', so every connection this integration makes is serialized process-wide."
- `CONNECT_TIMEOUT = 45.0`; `establish_connection(..., max_attempts=2)` after `close_stale_connections_by_address`; PIN written right after connect (`struct.pack(">I", pin)`, response=True); on PIN failure → `client.disconnect()`.
- `read_state()`: **"One connection: full state read, plus any queued writes. The Danfoss Eco is a sleepy battery device, so everything happens in a single connection per poll to minimise radio wake-ups."** Writes are read-modify-write against the just-read settings block, followed by `WRITE_SETTLE_S = 0.5` and a re-read; `finally: await client.disconnect()`.
- Retry ladder after a failed poll: `RETRY_BACKOFF_S = (60, 120, 300, 600, 900)`; an advertisement from the thermostat triggers an immediate retry (rate-limited `ADV_RETRY_MIN_INTERVAL_S = 60`). Default poll 15 min; docs: "Lower = fresher data but more battery use and BLE traffic. 10–30 min is sensible."
- DOCUMENTATION.md: "The device is a **deep-sleep power miser.** It is not always connectable; it wakes, advertises, and is reachable in bursts. This is why polling is infrequent by design." and "uses short-lived connections: connect → PIN → do the work → disconnect. This suits the device's sleepy nature and frees the proxy's limited connection slots between polls."
- PROTOCOL.md: advertises continuously as `<digit>;<mac>;eTRV`, leading digit "is live device state, observed values 0/2/4/6; 6 appears right after the timer button is pressed (pairing window open)"; PIN characteristic is plaintext `uint32 BE`; the secret-key characteristic "only exists in the GATT table while the pairing window is open" (so a cached service table can hide it — they `clear_cache()`); battery `0x2A19` unencrypted. Pairing wizard: "press again every ~10 s if it retries; the pairing window the device opens is short".
- CHANGELOG v1.1.0: `PARALLEL_UPDATES = 1` to serialize commands; "Read the weekly schedule within the same BLE connection as the rest of the state — one radio wake per poll instead of two."

### 4.5 jonbng/ha-danfoss-eco (HA integration, 2026-02) — contrasting (BlueZ-based) experience
- README "Important: Connection Timing": "Initial connection: ~27 seconds; GATT service discovery: ~30 seconds; Total time per operation: Up to 60-90 seconds ... The integration uses a 90-second connection timeout"; "ESPHome Bluetooth proxies have a hardcoded 30-second timeout for GATT operations ... `Timeout waiting for BluetoothGATTReadResponse after 30.0s`"; default poll 1 h; `stay_connected` option default False.
- `ble/client.py`: own retry loop inside a 90 s deadline: per-attempt timeout 20 s, exponential backoff 0.5 s → max 8 s with 35 % jitter, `close_stale_connections_by_address` every 8 s ("BlueZ can get stuck with org.bluez.Error.InProgress"), `max_attempts=1` per `establish_connection`; PIN sent once per connection then `asyncio.sleep(0.5)`; reads by hard-coded handle first (0x10 battery, 0x24 PIN, 0x2A settings, 0x2D temperature, 0x30 name, 0x3F key) "skipping service discovery speeds up operations significantly"; disconnects after each operation unless `stay_connected`.

### 4.6 NdS-Research-Facilities/DanfossECO2 (macOS CoreBluetooth, 2019), dsltip/Danfoss-BLE (CC2541 gateway, 2018), BenjaminSoelberg/danfoss-eco2 (firmware RE, 2022)
- dsltip README: "1) Before reading and writing data, PIN code must be send to danfoss. I'm sending four zero bytes to characteristic uuid_pin ... 3) Encryption key ... You are able to read it, only if you connect after pressing danfoss hard button." Its CC2541 firmware writes the PIN to handle 0x0024 right after link establishment and requests conn params min 400 / max 800 (×1.25 ms) / latency 0 / supervision timeout 600 (×10 ms = 6 s).
- NdS: stops scanning on discovery, connects, discovers services, writes PIN (`'0000'` as ASCII bytes), then reads; never disconnects (research script).
- BenjaminSoelberg: hardware is a PSoC 4100-BLE (BLE 4.2) running uC/OS-III; lists all characteristic UUIDs (incl. OTA service); no connection-timing notes.

### 4.7 GitHub search results (unauthenticated API)
`danfoss+eco+esphome` → only dmitry-cherkas/esphome-danfoss-eco. `danfoss+eco2+bluetooth` / `etrv+esp32` → jarikp/ESP32_DanfossECO2. `danfoss+eco+ble` → AdamStrojek/libetrv, keton/etrv2mqtt, dsltip/Danfoss-BLE, BenjaminSoelberg/danfoss-eco2. `danfoss+etrv` adds NdS-Research-Facilities/DanfossECO2, ussaka/Danfoss-Ally-HA-automations (Zigbee Ally, irrelevant), jonbng/ha-danfoss-eco, deltasystems-pl/danfoss-eco-ha. No other ESPHome/ESP32 Danfoss Eco component exists on GitHub.

---

## 5. ESPHome ble_client facts (from the local checkout `esphome/` (a local ESPHome git checkout, not part of the repository), `__version__ = "2026.10.0-dev"`) that bear on the component's lifecycle

Read directly from `components/ble_client/ble_client.cpp`, `components/esp32_ble_client/ble_client_base.cpp/.h`, `components/esp32_ble_tracker/esp32_ble_tracker.cpp`, `components/ble_device_base/ble_client_state.h`:

- `BLEClient::set_enabled(false)` → `if (enabled == this->enabled) return; ... ESP_LOGI("Disabling BLE client."); this->disconnect();`. `BLEClient::parse_device()` returns false while `!enabled`.
- `BLEClientBase::disconnect()`: if `IDLE`/`DISCONNECTING` → log and return; **if `CONNECTING` or `conn_id_ == UNSET_CONN_ID` → only sets `want_disconnect_ = true` and returns** (the pending `esp_ble_gattc_open` is not cancelled; it is closed when `OPEN_EVT` finally arrives); otherwise `unconditional_disconnect()` → `esp_ble_gattc_close()` → state `DISCONNECTING` with a **10 s safety timeout** (`DISCONNECTING_TIMEOUT`) after which `loop()` forces IDLE.
- `BLEClientBase::loop()` has **no timeout for the `CONNECTING` state**; only `INIT` (register app), `IDLE` (disable loop) and `DISCONNECTING` (10 s) are handled. A client parked in CONNECTING relies on Bluedroid's own open-timeout/`OPEN_EVT`.
- `BLEClientBase::connect()`: refuses (WARN "Connection already in progress") when `CONNECTING/CONNECTED/ESTABLISHED`, refuses when `DISCONNECTING` ("still waiting for CLOSE_EVT"), sets `CONNECTING` and calls `esp_ble_gattc_open(gattc_if_, remote_bda_, remote_addr_type_, true)` (direct connect). On immediate error → state IDLE.
- `ESP_GATTC_OPEN_EVT` with `status != OK` → `set_idle_()` ("Connection was never established so CLOSE_EVT may not follow"); with OK → `CONNECTED`, then `esp_ble_gattc_search_service`; `SEARCH_CMPL` → `set_state_internal_(ESTABLISHED)` (internal → **node_state is NOT updated**, which is why the component itself sets `node_state = ESTABLISHED` after PIN OK). `BLEClient::set_state()` (used for CONNECTING/IDLE/DISCONNECTING/DISCOVERED) overwrites `node_state` of every node.
- `ESP_GATTC_DISCONNECT_EVT` (link lost / remote closed) → `release_services()` + `DISCONNECTING` (waits for CLOSE_EVT; comment: transitioning to IDLE earlier "would allow reconnection before cleanup is complete, causing the controller to reject the new connection (status=133)"). `CLOSE_EVT` → IDLE.
- `parse_device()` (scanner-driven reconnect) requires `auto_connect_` (**YAML `auto_connect` default is `True`** in `ble_client/__init__.py`), matching address, state `IDLE` and a registered gattc_if; it sets `DISCOVERED`, and the tracker promotes **only one DISCOVERED client at a time** (`try_promote_discovered_clients_`: "Only promote the first discovered client to avoid multiple simultaneous connections"), stopping the scan first.
- The tracker restarts scanning only when no client is `CONNECTING`/`DISCOVERED`/`DISCONNECTING` (`loop()` gate), so a client stuck in CONNECTING also stalls scanning for every other client. While a connection is active it scans with a reduced "connection scan window".
- Connection parameters: V1 (plain `ble_client`) connections use ESP-IDF defaults; `MEDIUM_CONN_TIMEOUT` = 8 s supervision timeout is only applied for V3 (proxy) connections.
- `ble_before_disabled_event_handler()` (BLE stack going down, e.g. OTA) resets every client to INIT.

---

## 6. Consolidated facts about eTRV connection behaviour (only what sources state)

1. Every implementation uses **short-lived connections** (connect → PIN → reads/writes → disconnect); upstream author: "ESP32 does not maintain open connection with the Eco ... shutting down the BLE client once done" (#19, #4). etrv2mqtt documents that keeping the link open "May drain battery". Nobody documents the eTRV dropping an idle link on its own, nor a maximum session length.
2. The PIN (4 bytes, default 0000; upstream's `ac00844`: "etrv accepts pin code as hex bytes, not as string" — libetrv/deltasystems send `uint32 BE`, jarikp/NdS send ASCII digits, and both report success) must be written to `10020001-…` (handle 0x24) **once per connection** before any other characteristic is readable (dsltip, libetrv, deltasystems, jonbng, jarikp).
3. The secret-key characteristic exists in the GATT table only during the short pairing window after the timer button press (deltasystems; NdS/dsltip/libetrv `flags & 0x4`), and a cached service table can hide it.
4. Timing: through an ESP32 (Bluedroid) link, connect + discovery ≈ 3 s and reads < 1 s (deltasystems 2026-08; ryssel's #19 log: 6 s from attempt to Connected, then 4 reads within the same second). BlueZ hosts see 27–90 s (jonbng). FransOv (#11): "it can take several minutes to establish communication" at > 5 m.
5. Status 133 / 0x85 (`ESP_GATT_ERROR`) on open is the common failure (upstream #4, `810328d`, ryssel README, ESPHome comment about reconnecting before cleanup completes).
6. Multiple eTRVs on one radio: ESPHome ble_client is limited to 3 (#5); marcin7d (#11) saw freezes with 2–3 valves; ryssel found the raw `esp_ble_gap_stop_scanning()` blocked the second device (`2cd31a1`); both ryssel (global connect slot + 700 ms cooldown + 1 s per-device throttle + MAC-derived 0.2–3.2 s poll spread) and deltasystems (process-wide lock, `PARALLEL_UPDATES = 1`) and NicoJorgensen1 (minute slots) serialize connections.
7. Writing the settings block with stale/partial data corrupts the eTRV (mode switch "resets the valve", "heating to 60 C", battery pull needed) — cause identified by ryssel as the 8-vs-16-byte encryption bug; every write must be preceded by a fresh read in the same session (deltasystems does read-modify-write inside one connection).
8. Nobody in the ecosystem reported the *client* side hanging in CONNECTING as a distinct issue; the reported hangs are (a) #11 "sometimes ... does not get to disconnect the ble" (2022.10 API-transition builds) and (b) #18 endless "Attempting BLE connection" after the 2023.11 state-machine change.

---

## 7. What the local merged copy lacks / differs from the other authors' lifecycle code

Relative to `components/danfoss_eco` (commit `702c012` + current uncommitted diagnostics/90 s watchdog):

| Item | ryssel master (Jul 2026) | local |
|---|---|---|
| Timeout on outstanding requests | `request_timeout` (default 15 s, clamp 1–60 s) watchdog started at the first sent request; on expiry: teardown, clear queue, backoff level +1 (timeout ×2 up to 60 s, max level 5), backoff preserved across the resulting disconnect | uncommitted 90 s `CONNECTION_WATCHDOG_MS` measured from `CONNECT_EVT`, counter reset, `set_enabled(false)`/`disconnect()`; no backoff, queue not cleared |
| `request_counter_` reset on link loss | `teardown_connection_()` zeroes it on `DISCONNECT_EVT`, on status error, on watchdog | only logs "counter is now STALE" on CLOSE/DISCONNECT (uncommitted); counter otherwise persists → next session starts with a non-zero counter |
| Underflow guard | `on_read/on_write` return early if counter already 0 | none (`uint8_t` wraps to 255 → the component then never sees counter==0 and never disconnects) |
| Overflow guard | counter == 0xFF → teardown | none |
| Reconnect while work is queued | `loop()`: `IDLE && !commands_.empty()` → `connect()` | none (queued writes wait for the next `update()`/`control()`) |
| Connect throttling | ≥ 1 s between `connect()` attempts per device; global single connect slot with 700 ms cooldown after release; 5 s per-device cooldown after failed OPEN | none |
| Poll spreading | per-device 200–3199 ms offset (from MAC) applied to every `update()` | none |
| Post-write re-read | `request_device_state_()` (no poll re-scheduling) | `update()` (also fine, no scheduler in local) |
| `on_write_pin` failure | unchanged in both: `disconnect()` + `mark_failed()` (component permanently dead until reboot) | same |
| Failed OPEN handling | log + 5 s cooldown, client stays enabled | log only, client stays enabled (revert `702c012`) |
| `auto_connect` guidance | README recommends `auto_connect: false` for this component | local device.cpp comment relies on `auto_connect` (default true) for recovery |
| Queue | FreeRTOS queue (blocking push) with `size()`/`clear()` | `LockFreeQueue` (non-blocking, drops when full, no `clear()`) |

Things no fork has: cancellation of a client stuck in `CONNECTING` (ESPHome itself has no CONNECTING timeout; `disconnect()` in that state only sets `want_disconnect_`); explicit handling of `DISCONNECTING`; any use of `esp_ble_gap_update_conn_params` for the eTRV.

## 7. Update 2026-09-29: forks of forks, later commits

The network has 22 forks: 17 direct ones (section 1) and 5 forks of forks that the survey above
missed: borstel4711, dhewson and Drak63 (of ckoca), MindTwister and lars-ryssel-trackman (of
ryssel). All branches were fetched and compared with what this repository already does.

| Fork | Unique commits | What they do | Taken over |
|---|---|---|---|
| borstel4711 | 14 (master + 4 branches, until 2026-07-19) | sequential connection manager with a session watchdog, summer mode (poll every 2 h while an HA heating entity is `off`), PoE gateway example with an external antenna, scanner log dedupe, no-op deleter for the `shared_ptr` to the component, stop after 5 PIN rejections | scanner: each eTRV logged once (+ pairing window changes); non-owning `shared_ptr`; a rejected PIN no longer forces a service rediscovery when the handles came from the eTRV itself (the cost that the 5-rejection limit addresses) |
| dhewson, Drak63 | 1, 7 | `address_str()` / `to_str()` compile fixes for newer ESPHome | already present |
| MindTwister | 2 | reformat, inlined XXTEA (keeps upstream's 8-of-16-byte settings write bug) | nothing |
| ryssel | 4 beyond the merged state | request watchdog with back-off, global connect slot with 700 ms gap, 5 s cooldown after a failed open, `auto_connect: false` advice | already covered by `request_timeout`, the tracker promotion and the measured retry policy (MERGE_NOTES, fifth pass); `auto_connect` stays on on purpose |

No fork suggests a different protocol layout: all of them use the same bytes for set point / room
temperature, settings (flags, min/max/frost, mode, vacation) and the error bitfield. None sets
connection parameters, MTU or TX power. Not taken over: the summer mode (it can be built in YAML with
`component.resume` + a templated `update_interval`; the reference configuration polls every 12 h
anyway) and the extra gap between two eTRVs' connections (neither fork measured a benefit). The PoE
gateway with an external antenna is worth trying where the signal is weak: it targets exactly the
limit measured in MERGE_NOTES (the ESP hearing the eTRV's sparse advertising), and Ethernet removes
the WiFi/Bluetooth sharing of the radio.
