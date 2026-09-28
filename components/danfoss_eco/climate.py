import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, ble_client, sensor, binary_sensor, text_sensor, time
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_ICON,

    CONF_TEMPERATURE,
    CONF_BATTERY_LEVEL,

    CONF_ENTITY_CATEGORY,
    ENTITY_CATEGORY_DIAGNOSTIC,

    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
    UNIT_CELSIUS,

    CONF_DEVICE_CLASS,
    CONF_TIME_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_TIMESTAMP,
    DEVICE_CLASS_PROBLEM
)

CODEOWNERS = ["@dmitry-cherkas", "@inteltryb93"]
DEPENDENCIES = ["ble_client"]
# load zero-configuration dependencies automatically
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "esp32_ble_tracker"]

CONF_PIN_CODE = 'pin_code'
CONF_SECRET_KEY = 'secret_key'
CONF_PROBLEMS = 'problems'
CONF_PROBLEMS_DETAIL = 'problems_detail'
CONF_REQUEST_TIMEOUT = 'request_timeout'
CONF_RETRY_WINDOW = 'retry_window'
CONF_CONNECTION = 'connection'
CONF_CACHE_SERVICES = 'cache_services'
CONF_PROBLEMS_DETAIL_DEFAULT_ICON = 'mdi:format-list-checks'

eco_ns = cg.esphome_ns.namespace("danfoss_eco")
DanfossEco = eco_ns.class_(
    "Device", climate.Climate, ble_client.BLEClientNode, cg.PollingComponent
)

# ---------------------------------------------------------------------------------------------
# Optional entities. Everything the eTRV reports over Bluetooth can be exposed (see README
# "Bluetooth characteristics"); add only the keys you want. All of them come from reads the
# component does anyway, except the "information" ones: those are read once after boot (the
# schedule again once a day), and only when the corresponding key is configured.
# key: (setter, schema)
# ---------------------------------------------------------------------------------------------
def _diag_binary(icon=cv.UNDEFINED, device_class=cv.UNDEFINED):
    return binary_sensor.binary_sensor_schema(
        icon=icon, device_class=device_class, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    )


def _diag_temperature(icon=cv.UNDEFINED):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        icon=icon,
        device_class=DEVICE_CLASS_TEMPERATURE,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _diag_timestamp(icon):
    return sensor.sensor_schema(
        icon=icon, device_class=DEVICE_CLASS_TIMESTAMP, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    )


def _diag_text(icon):
    return text_sensor.text_sensor_schema(icon=icon, entity_category=ENTITY_CATEGORY_DIAGNOSTIC)


BINARY_SENSORS = {
    # error flags (characteristic 10020009); the `problems` sensor covers all of them
    "low_battery": ("set_low_battery", _diag_binary(device_class=DEVICE_CLASS_BATTERY)),       # E14/E15
    "valve_error": ("set_valve_error", _diag_binary("mdi:valve-closed", DEVICE_CLASS_PROBLEM)),  # E9
    "motor_error": ("set_motor_error", _diag_binary("mdi:engine-off", DEVICE_CLASS_PROBLEM)),   # E6
    "clock_error": ("set_clock_error", _diag_binary("mdi:clock-alert-outline", DEVICE_CLASS_PROBLEM)),  # E10
    "hardware_error": ("set_hardware_error", _diag_binary("mdi:chip", DEVICE_CLASS_PROBLEM)),   # E1-E5, E7, E8, E11-E13
    # settings (characteristic 10020003)
    "child_lock": ("set_child_lock", _diag_binary("mdi:lock-outline")),
    "valve_installed": ("set_valve_installed", _diag_binary("mdi:radiator")),
    "daylight_saving": ("set_daylight_saving", _diag_binary("mdi:sun-clock-outline")),
    "adaptive_learning": ("set_adaptive_learning", _diag_binary("mdi:brain")),
    "slow_regulation": ("set_slow_regulation", _diag_binary("mdi:speedometer-slow")),
    "vertical_installation": ("set_vertical_installation", _diag_binary("mdi:swap-vertical")),
    "display_flip": ("set_display_flip", _diag_binary("mdi:screen-rotation")),
    # information (characteristic 10020002, read once after boot; the PIN itself is never exposed)
    "pin_protection": ("set_pin_protection", _diag_binary("mdi:form-textbox-password")),
}

SENSORS = {
    "temperature_min": ("set_temperature_min", _diag_temperature("mdi:thermometer-chevron-down")),
    "temperature_max": ("set_temperature_max", _diag_temperature("mdi:thermometer-chevron-up")),
    "frost_protection_temperature": ("set_frost_protection_temperature", _diag_temperature("mdi:snowflake-thermometer")),
    "vacation_temperature": ("set_vacation_temperature", _diag_temperature("mdi:airplane")),
    "vacation_start": ("set_vacation_start", _diag_timestamp("mdi:airplane-takeoff")),
    "vacation_end": ("set_vacation_end", _diag_timestamp("mdi:airplane-landing")),
    # weekly schedule (characteristics 1002000D-F)
    "schedule_home_temperature": ("set_schedule_home_temperature", _diag_temperature("mdi:home-thermometer")),
    "schedule_away_temperature": ("set_schedule_away_temperature", _diag_temperature("mdi:home-export-outline")),
}

TEXT_SENSORS = {
    # manual / schedule / vacation / pause (the climate entity shows HEAT / AUTO / OFF)
    "device_mode": ("set_device_mode", text_sensor.text_sensor_schema(icon="mdi:thermostat")),
    "schedule": ("set_schedule", _diag_text("mdi:calendar-clock")),
    "thermostat_name": ("set_thermostat_name", _diag_text("mdi:label-outline")),
}

# Device Information Service (0x180A), read once after boot: key -> InfoField index
INFO_TEXT_SENSORS = {
    "manufacturer": (0, "mdi:factory"),
    "model": (1, "mdi:information-outline"),
    "serial_number": (2, "mdi:barcode"),
    "hardware_version": (3, "mdi:chip"),
    "firmware_version": (4, "mdi:package-up"),
    "software_version": (5, "mdi:package-variant"),
}


def validate_secret(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9a-fA-F]{32}", value):
        raise cv.Invalid("Secret key should be exactly 16 bytes written as 32 hex characters")
    return value

def validate_pin(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9]{4}", value):
        raise cv.Invalid("PIN code should be exactly 4 digits")
    return value

# NOTE: climate.climate_schema(DanfossEco) replaces the removed climate.CLIMATE_SCHEMA and already
# declares the component id (cv.GenerateID()) and a built-in `visual:` block (min/max/step), so we
# neither add cv.GenerateID() nor a custom visual schema here. binary_sensor.BINARY_SENSOR_SCHEMA was
# removed in favour of binary_sensor.binary_sensor_schema().
CONFIG_SCHEMA = (
    climate.climate_schema(DanfossEco).extend(
        {
            # Mark credentials as sensitive for deterministic redaction in config dumps/logs
            # (replaces the legacy substring heuristic, removed in ESPHome 2026.12.0).
            cv.Optional(CONF_SECRET_KEY): cv.sensitive(validate_secret),
            cv.Optional(CONF_PIN_CODE): cv.sensitive(validate_pin),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # ON while the eTRV reports any error code (E1-E16, see README). E10 "invalid clock" is
            # cleared automatically when `time_id` is configured.
            cv.Optional(CONF_PROBLEMS): binary_sensor.binary_sensor_schema().extend({
                cv.Optional(CONF_NAME): cv.string,
                cv.Optional(CONF_ENTITY_CATEGORY, default=ENTITY_CATEGORY_DIAGNOSTIC): cv.entity_category,
                cv.Optional(CONF_DEVICE_CLASS, default=DEVICE_CLASS_PROBLEM): binary_sensor.validate_device_class
            }),
            cv.Optional(CONF_PROBLEMS_DETAIL): text_sensor.text_sensor_schema().extend({
                cv.Optional(CONF_NAME): cv.string,
                cv.Optional(CONF_ENTITY_CATEGORY, default=ENTITY_CATEGORY_DIAGNOSTIC): cv.entity_category,
                cv.Optional(CONF_ICON, default=CONF_PROBLEMS_DETAIL_DEFAULT_ICON): cv.icon,
            }),
            # No GATT response within this time -> the BLE link is torn down (and retried).
            cv.Optional(CONF_REQUEST_TIMEOUT, default="15s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=2), max=cv.TimePeriod(seconds=60)),
            ),
            # Diagnostic: ON after a successful read, OFF when the eTRV has not been reachable for
            # retry_window (requests are kept and retried) or after a protocol error.
            cv.Optional(CONF_CONNECTION): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Optional time source: keeps the eTRV clock set (needed for AUTO / schedule mode and
            # vacations) and acknowledges E10 "invalid clock" after a battery change.
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # Length of the fast retry phase (back-off 3/15/30/60/120 s). Afterwards requests are
            # kept and retried every 5 min (every 15 min after one more hour), for up to 24 h.
            cv.Optional(CONF_RETRY_WINDOW, default="10min"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=30), max=cv.TimePeriod(minutes=60)),
            ),
            # Keep the eTRV's GATT service table in flash (Bluedroid's NVS service cache, the same
            # thing bluetooth_proxy's `cache_services` does). Without it ESPHome clears the table after
            # every disconnect and the Bluetooth stack rediscovers all services on every link before
            # the first request goes out (~2 s, >10 s at a weak signal - most of each link's time).
            cv.Optional(CONF_CACHE_SERVICES, default=True): cv.boolean,
            **{cv.Optional(key): schema for key, (_, schema) in BINARY_SENSORS.items()},
            **{cv.Optional(key): schema for key, (_, schema) in SENSORS.items()},
            **{cv.Optional(key): schema for key, (_, schema) in TEXT_SENSORS.items()},
            **{cv.Optional(key): _diag_text(icon) for key, (_, icon) in INFO_TEXT_SENSORS.items()},
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.polling_component_schema("30min"))
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_secret_key(config.get(CONF_SECRET_KEY, "")))
    cg.add(var.set_pin_code(config.get(CONF_PIN_CODE, "")))
    cg.add(var.set_request_timeout(config[CONF_REQUEST_TIMEOUT].total_milliseconds))
    cg.add(var.set_retry_window(config[CONF_RETRY_WINDOW].total_milliseconds))
    if config[CONF_CACHE_SERVICES]:
        from esphome.components.esp32 import add_idf_sdkconfig_option

        add_idf_sdkconfig_option("CONFIG_BT_GATTC_CACHE_NVS_FLASH", True)

    if CONF_BATTERY_LEVEL in config:
        sens = await sensor.new_sensor(config[CONF_BATTERY_LEVEL])
        cg.add(var.set_battery_level(sens))
    if CONF_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_TEMPERATURE])
        cg.add(var.set_temperature(sens))
    if CONF_PROBLEMS in config:
        b_sens = await binary_sensor.new_binary_sensor(config[CONF_PROBLEMS])
        cg.add(var.set_problems(b_sens))
    if CONF_CONNECTION in config:
        b_sens = await binary_sensor.new_binary_sensor(config[CONF_CONNECTION])
        cg.add(var.set_connection(b_sens))
    if CONF_TIME_ID in config:
        time_ = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(time_))
    if CONF_PROBLEMS_DETAIL in config:
        t_sens = await text_sensor.new_text_sensor(config[CONF_PROBLEMS_DETAIL])
        cg.add(var.set_problems_detail(t_sens))

    for key, (setter, _) in BINARY_SENSORS.items():
        if key in config:
            b_sens = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(b_sens))
    for key, (setter, _) in SENSORS.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))
    for key, (setter, _) in TEXT_SENSORS.items():
        if key in config:
            t_sens = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(t_sens))
    for key, (field, _) in INFO_TEXT_SENSORS.items():
        if key in config:
            t_sens = await text_sensor.new_text_sensor(config[key])
            cg.add(var.set_info_sensor(field, t_sens))
