#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
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

        class MyComponent : public Climate, public PollingComponent
        {
        public:
            float get_setup_priority() const override { return setup_priority::DATA; }

            void dump_config() override
            {
                LOG_CLIMATE("", "Danfoss Eco eTRV", this);
                LOG_SENSOR("", "Battery Level", this->battery_level_);
                LOG_SENSOR("", "Room Temperature", this->temperature_);
                LOG_BINARY_SENSOR("", "Problems", this->problems_);
            }

            ClimateTraits traits() override
            {
                auto traits = ClimateTraits();
                traits.set_supports_current_temperature(true);

                traits.set_supported_modes(set<ClimateMode>({ClimateMode::CLIMATE_MODE_HEAT, ClimateMode::CLIMATE_MODE_AUTO}));
                traits.set_visual_temperature_step(0.5);

                traits.set_supports_current_temperature(true); // supports reporting current temperature
                traits.set_supports_action(true);              // supports reporting current action
                return traits;
            }

            void set_battery_level(Sensor *battery_level) { battery_level_ = battery_level; }
            void set_temperature(Sensor *temperature) { temperature_ = temperature; }
            void set_problems(BinarySensor *problems) { problems_ = problems; }

            Sensor *battery_level() { return this->battery_level_; }
            Sensor *temperature() { return this->temperature_; }
            BinarySensor *problems() { return this->problems_; }

        protected:
            Sensor *battery_level_{nullptr};
            Sensor *temperature_{nullptr};
            BinarySensor *problems_{nullptr};
        };

    } // namespace danfoss_eco
} // namespace esphome