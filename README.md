Danfoss Eco (merged, ESPHome 2026.x / ESP-IDF)
==============================================

The ``danfoss_eco`` climate platform creates a climate device which can be used to control a
Danfoss Eco eTRV over Bluetooth Low Energy from an ESP32.

This is a **merge of the [`ryssel`](https://github.com/ryssel/esphome-danfoss-eco) and
[`ckoca`](https://github.com/ckoca/esphome-danfoss-eco) forks** of
[`dmitry-cherkas/esphome-danfoss-eco`](https://github.com/dmitry-cherkas/esphome-danfoss-eco),
updated to build on **ESPHome 2026.6.3** with the **ESP-IDF** framework. See
[`MERGE_NOTES.md`](MERGE_NOTES.md) for the detailed, per-file merge rationale.

This component supports:

- Switch between Manual and Schedule modes
- Set the target room temperature
- Show the current temperature
- Show current action (Heating/Idle)
- Show remaining battery level
- Managing multiple eTRVs from a single ESP32
- Reporting eTRV error status (binary `problems` sensor and human-readable `problems_detail` text sensor)

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
> The component compiles warning-free on the default ESP-IDF (5.5.x) and has been verified clean on
> **ESP-IDF 6.0.1** (GCC 15.2.0, ESP32-C3). To pin that version add `version: 6.0.1` under `framework:`.

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
    update_interval: 30min
    battery_level:
      name: "My Room eTRV Battery Level"
    temperature:
      name: "My Room eTRV Temperature"
    problems:
      name: "My Room eTRV Problems"
    problems_detail:
      name: "My Room eTRV Problems Detail"
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
- **pin_code** (*Optional*, string): Device PIN code (if configured). 4-character numeric string.
- **secret_key** (*Required*, string): Device encryption key, 16 bytes / 32 hex characters.
- **battery_level** (*Optional*): Remaining battery level sensor.
- **temperature** (*Optional*): Current temperature (°C) sensor.
- **problems** (*Optional*): Binary problem sensor (any fault active).
- **problems_detail** (*Optional*): Text sensor listing active faults
  (`Valve Stuck | Invalid Time | Low Battery | Very Low Battery`).
- **visual** (*Optional*): Standard ESPHome climate `visual:` block (min/max temperature, step).
  The component also auto-updates the displayed range from the eTRV's reported settings.

> **NOTE:** A complete minimal validation config is in `test_merged.yaml`.

See Also
--------

This component is based on the work of other authors:
* [AdamStrojek libetrv](https://github.com/AdamStrojek/libetrv) (with additional features from [spin83](https://github.com/spin83/libetrv) fork)
* MQTT bridge by [keton](https://github.com/keton/etrv2mqtt) and Home Assistant add-on by [HBDK](https://github.com/HBDK/Eco2-Tools)
* XXTEA implementation by [boseji](https://github.com/boseji) (vendored as `xxtea-lib.h` / `xxtea_core.*`)
