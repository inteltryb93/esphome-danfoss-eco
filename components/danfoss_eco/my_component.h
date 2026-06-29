#pragma once

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

            // Updates the visual gauge range (called with the device-reported min/max after a read).
            void set_temperature_range(float min_temp, float max_temp)
            {
                this->visual_min_temperature_ = min_temp;
                this->visual_max_temperature_ = max_temp;
            }

            void set_battery_level(Sensor *battery_level) { battery_level_ = battery_level; }
            void set_temperature(Sensor *temperature) { temperature_ = temperature; }
            void set_problems(BinarySensor *problems) { problems_ = problems; }
            void set_problems_detail(TextSensor *problems_detail) { problems_detail_ = problems_detail; }

            Sensor *battery_level() { return this->battery_level_; }
            Sensor *temperature() { return this->temperature_; }
            BinarySensor *problems() { return this->problems_; }
            TextSensor *problems_detail() { return this->problems_detail_; }

            virtual void set_secret_key(uint8_t *, bool) = 0;

        protected:
            Sensor *battery_level_{nullptr};
            Sensor *temperature_{nullptr};
            BinarySensor *problems_{nullptr};
            TextSensor *problems_detail_{nullptr};

            float visual_min_temperature_{5.0f};
            float visual_max_temperature_{30.0f};
        };

    } // namespace danfoss_eco
} // namespace esphome
