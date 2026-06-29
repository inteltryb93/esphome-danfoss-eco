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
| `command.h` | **ckoca** `LockFreeQueue` + explicit size | `esp32_ble_tracker::Queue` was removed. `LockFreeQueue` is the official, lock-free, **non-blocking** replacement (stores `T*`, matching `push(new Command)`/`pop()`/`delete`). ryssel's FreeRTOS wrapper used `xQueueSend(..., portMAX_DELAY)` — a **blocking** push in the main loop (watchdog risk). A local `COMMAND_QUEUE_SIZE = 16` is defined (the framework's `MAX_BLE_QUEUE_SIZE` is for BLE events, wrong semantics). |
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
