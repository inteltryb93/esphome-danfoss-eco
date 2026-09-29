#pragma once

#include <algorithm>

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/climate/climate.h"

#include "helpers.h"

namespace esphome
{
    namespace danfoss_eco
    {
        using namespace std;
        using namespace esphome::climate;
        using namespace esphome::sensor;
        using namespace esphome::binary_sensor;
        using namespace esphome::text_sensor;

        // Device Information Service strings (0x180A), each an optional text sensor.
        enum InfoField : uint8_t
        {
            INFO_MANUFACTURER = 0,
            INFO_MODEL,
            INFO_SERIAL,
            INFO_HARDWARE,
            INFO_FIRMWARE,
            INFO_SOFTWARE,
            INFO_COUNT
        };

        class MyComponent : public Climate, public PollingComponent, public enable_shared_from_this<MyComponent>
        {
        public:
            float get_setup_priority() const override { return setup_priority::DATA; }

            ClimateTraits traits() override
            {
                auto traits = ClimateTraits();

                // ESPHome 2026.x ClimateTraits API:
                //  - set_supports_current_temperature()/set_supports_action() were REMOVED; the
                //    capabilities are now expressed via feature flags.
                //  - set_supported_modes(set<...>) no longer accepts a std::set; use add_supported_mode().
                traits.add_feature_flags(CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | CLIMATE_SUPPORTS_ACTION);
                // HEAT = manual set point, AUTO = weekly schedule (a vacation also shows as AUTO),
                // OFF = the eTRV "pause" (heating off, frost protection only).
                traits.add_supported_mode(ClimateMode::CLIMATE_MODE_OFF);
                traits.add_supported_mode(ClimateMode::CLIMATE_MODE_HEAT);
                traits.add_supported_mode(ClimateMode::CLIMATE_MODE_AUTO);
                traits.set_visual_temperature_step(0.5);

                // Visual gauge range. Defaults to the eTRV's physical range (5-30 C) and is updated
                // from the device's reported min/max settings via set_temperature_range(). A YAML
                // `visual:` block, if present, still overrides this (applied by the climate core).
                traits.set_visual_min_temperature(this->visual_min_temperature_);
                traits.set_visual_max_temperature(this->visual_max_temperature_);
                return traits;
            }

            // Updates the visual gauge range (called with the device-reported min/max after a read),
            // never beyond what control() accepts (5-30 C).
            void set_temperature_range(float min_temp, float max_temp)
            {
                min_temp = std::max(5.0f, min_temp);
                max_temp = std::min(30.0f, max_temp);
                if (min_temp >= max_temp)
                    return;
                this->visual_min_temperature_ = min_temp;
                this->visual_max_temperature_ = max_temp;
            }

            void set_battery_level(Sensor *battery_level) { battery_level_ = battery_level; }
            void set_temperature(Sensor *temperature) { temperature_ = temperature; }
            void set_problems(BinarySensor *problems) { problems_ = problems; }
            void set_problems_detail(TextSensor *problems_detail) { problems_detail_ = problems_detail; }
            void set_connection(BinarySensor *connection) { connection_ = connection; }

            // error flags
            void set_low_battery(BinarySensor *s) { low_battery_ = s; }
            void set_valve_error(BinarySensor *s) { valve_error_ = s; }
            void set_motor_error(BinarySensor *s) { motor_error_ = s; }
            void set_clock_error(BinarySensor *s) { clock_error_ = s; }
            void set_hardware_error(BinarySensor *s) { hardware_error_ = s; }
            // settings
            void set_device_mode(TextSensor *s) { device_mode_ = s; }
            void set_temperature_min(Sensor *s) { temperature_min_ = s; }
            void set_temperature_max(Sensor *s) { temperature_max_ = s; }
            void set_frost_protection_temperature(Sensor *s) { frost_protection_temperature_ = s; }
            void set_vacation_temperature(Sensor *s) { vacation_temperature_ = s; }
            void set_vacation_start(Sensor *s) { vacation_start_ = s; }
            void set_vacation_end(Sensor *s) { vacation_end_ = s; }
            void set_child_lock(BinarySensor *s) { child_lock_ = s; }
            void set_valve_installed(BinarySensor *s) { valve_installed_ = s; }
            void set_daylight_saving(BinarySensor *s) { daylight_saving_ = s; }
            void set_adaptive_learning(BinarySensor *s) { adaptive_learning_ = s; }
            void set_slow_regulation(BinarySensor *s) { slow_regulation_ = s; }
            void set_vertical_installation(BinarySensor *s) { vertical_installation_ = s; }
            void set_display_flip(BinarySensor *s) { display_flip_ = s; }
            // information
            void set_pin_protection(BinarySensor *s) { pin_protection_ = s; }
            void set_thermostat_name(TextSensor *s) { thermostat_name_ = s; }
            void set_schedule(TextSensor *s) { schedule_ = s; }
            void set_schedule_home_temperature(Sensor *s) { schedule_home_temperature_ = s; }
            void set_schedule_away_temperature(Sensor *s) { schedule_away_temperature_ = s; }
            void set_info_sensor(uint8_t field, TextSensor *s)
            {
                if (field < INFO_COUNT)
                    info_sensors_[field] = s;
            }

            Sensor *battery_level() { return this->battery_level_; }
            Sensor *temperature() { return this->temperature_; }
            BinarySensor *problems() { return this->problems_; }
            TextSensor *problems_detail() { return this->problems_detail_; }
            BinarySensor *low_battery() { return this->low_battery_; }
            BinarySensor *valve_error() { return this->valve_error_; }
            BinarySensor *motor_error() { return this->motor_error_; }
            BinarySensor *clock_error() { return this->clock_error_; }
            BinarySensor *hardware_error() { return this->hardware_error_; }
            TextSensor *device_mode() { return this->device_mode_; }
            Sensor *temperature_min() { return this->temperature_min_; }
            Sensor *temperature_max() { return this->temperature_max_; }
            Sensor *frost_protection_temperature() { return this->frost_protection_temperature_; }
            Sensor *vacation_temperature() { return this->vacation_temperature_; }
            Sensor *vacation_start() { return this->vacation_start_; }
            Sensor *vacation_end() { return this->vacation_end_; }
            BinarySensor *child_lock() { return this->child_lock_; }
            BinarySensor *valve_installed() { return this->valve_installed_; }
            BinarySensor *daylight_saving() { return this->daylight_saving_; }
            BinarySensor *adaptive_learning() { return this->adaptive_learning_; }
            BinarySensor *slow_regulation() { return this->slow_regulation_; }
            BinarySensor *vertical_installation() { return this->vertical_installation_; }
            BinarySensor *display_flip() { return this->display_flip_; }
            BinarySensor *pin_protection() { return this->pin_protection_; }
            TextSensor *thermostat_name() { return this->thermostat_name_; }
            TextSensor *schedule() { return this->schedule_; }
            Sensor *schedule_home_temperature() { return this->schedule_home_temperature_; }
            Sensor *schedule_away_temperature() { return this->schedule_away_temperature_; }
            TextSensor *info_sensor(uint8_t field) { return field < INFO_COUNT ? this->info_sensors_[field] : nullptr; }

            virtual void set_secret_key(uint8_t *, bool) = 0;

            // Publishes the climate state after the properties updated it from a device read. The Device
            // overrides this to keep showing values that are requested but not yet written.
            virtual void publish_climate_state() { this->publish_state(); }
            // A read response changed the climate fields. The Device publishes them once, when the
            // whole read batch is in (or the link ends), so that the set point and the mode of one
            // read never reach Home Assistant separately (e.g. the frost set point still with HEAT).
            void mark_climate_changed() { this->climate_changed_ = true; }

        protected:
            Sensor *battery_level_{nullptr};
            Sensor *temperature_{nullptr};
            BinarySensor *problems_{nullptr};
            TextSensor *problems_detail_{nullptr};
            BinarySensor *connection_{nullptr};

            BinarySensor *low_battery_{nullptr};
            BinarySensor *valve_error_{nullptr};
            BinarySensor *motor_error_{nullptr};
            BinarySensor *clock_error_{nullptr};
            BinarySensor *hardware_error_{nullptr};

            TextSensor *device_mode_{nullptr};
            Sensor *temperature_min_{nullptr};
            Sensor *temperature_max_{nullptr};
            Sensor *frost_protection_temperature_{nullptr};
            Sensor *vacation_temperature_{nullptr};
            Sensor *vacation_start_{nullptr};
            Sensor *vacation_end_{nullptr};
            BinarySensor *child_lock_{nullptr};
            BinarySensor *valve_installed_{nullptr};
            BinarySensor *daylight_saving_{nullptr};
            BinarySensor *adaptive_learning_{nullptr};
            BinarySensor *slow_regulation_{nullptr};
            BinarySensor *vertical_installation_{nullptr};
            BinarySensor *display_flip_{nullptr};

            BinarySensor *pin_protection_{nullptr};
            TextSensor *thermostat_name_{nullptr};
            TextSensor *schedule_{nullptr};
            Sensor *schedule_home_temperature_{nullptr};
            Sensor *schedule_away_temperature_{nullptr};
            TextSensor *info_sensors_[INFO_COUNT]{};

            bool climate_changed_{false};

            float visual_min_temperature_{5.0f};
            float visual_max_temperature_{30.0f};
        };

    } // namespace danfoss_eco
} // namespace esphome
