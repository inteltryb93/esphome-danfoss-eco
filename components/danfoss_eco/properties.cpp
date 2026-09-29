#include "esphome/components/climate/climate.h"
#include "esphome/core/log.h"

#include "properties.h"
#include "helpers.h"

#ifdef USE_ESP32

namespace esphome
{
    namespace danfoss_eco
    {
        bool DeviceProperty::init_handle(BLEClient *client)
        {
            // ESPBTUUID::to_string() is deprecated (removed in ESPHome 2026.8.0). Use to_str(buf),
            // which writes into a caller-provided char[UUID_STR_LEN] buffer and returns it.
            char svc_buf[UUID_STR_LEN], chr_buf[UUID_STR_LEN];
            ESP_LOGV(TAG, "[%s] resolving handler for service=%s, characteristic=%s", this->component_->get_name().c_str(), this->service_uuid.to_str(svc_buf), this->characteristic_uuid.to_str(chr_buf));
            this->handle = INVALID_HANDLE;
            auto chr = client->get_characteristic(this->service_uuid, this->characteristic_uuid);
            if (chr == nullptr)
            {
                char buf[UUID_STR_LEN];
                if (this->essential())
                    ESP_LOGW(TAG, "[%s] characteristic uuid=%s not found", this->component_->get_name().c_str(), this->characteristic_uuid.to_str(buf));
                else
                    ESP_LOGD(TAG, "[%s] optional characteristic uuid=%s not present", this->component_->get_name().c_str(), this->characteristic_uuid.to_str(buf));
                return false;
            }

            this->handle = chr->handle;
            return true;
        }

        bool DeviceProperty::read_request(BLEClient *client)
        {
            if (!this->has_handle())
            {
                ESP_LOGW(TAG, "[%s] read_request: characteristic handle not resolved, skipping", this->component_->get_name().c_str());
                return false;
            }
            ESP_LOGV(TAG, "[%s] read_request: handle=%#04x conn_id=%d", this->component_->get_name().c_str(), this->handle, client->get_conn_id());
            auto status = esp_ble_gattc_read_char(client->get_gattc_if(),
                                                  client->get_conn_id(),
                                                  this->handle,
                                                  ESP_GATT_AUTH_REQ_NONE);
            if (status != ESP_OK)
                ESP_LOGW(TAG, "[%s] esp_ble_gattc_read_char failed, handle=%#04x, status=%01x", this->component_->get_name().c_str(), this->handle, status);

            return status == ESP_OK;
        }

        bool WritableProperty::write_request(BLEClient *client, uint8_t *data, uint16_t data_len, bool log_data)
        {
            if (!this->has_handle())
            {
                ESP_LOGW(TAG, "[%s] write_request: characteristic handle not resolved, skipping", this->component_->get_name().c_str());
                return false;
            }
            ESP_LOGV(TAG, "[%s] write_request: handle=%#04x, data=%s", this->component_->get_name().c_str(), this->handle, log_data ? format_hex_pretty(data, data_len).c_str() : "(not logged)");

            auto status = esp_ble_gattc_write_char(client->get_gattc_if(),
                                                   client->get_conn_id(),
                                                   this->handle,
                                                   data_len,
                                                   data,
                                                   ESP_GATT_WRITE_TYPE_RSP,
                                                   ESP_GATT_AUTH_REQ_NONE);
            if (status != ESP_OK)
                ESP_LOGW(TAG, "[%s] esp_ble_gattc_write_char failed, handle=%#04x, status=%01x", this->component_->get_name().c_str(), this->handle, status);

            return status == ESP_OK;
        }

        static void publish_binary(BinarySensor *s, bool state)
        {
            if (s != nullptr)
                s->publish_state(state);
        }

        static void publish_sensor(Sensor *s, float state)
        {
            if (s != nullptr)
                s->publish_state(state);
        }

        bool BatteryProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            uint8_t battery_level = value[0];
            if (battery_level > 100)
            {
                // Not a protocol error (it must never make a transaction give up and drop a requested
                // set point): the level is simply unknown this time.
                ESP_LOGW(TAG, "[%s] battery level %u %% out of range - shown as unknown", this->component_->get_name().c_str(), battery_level);
                publish_sensor(this->component_->battery_level(), NAN);
                return true;
            }
            ESP_LOGD(TAG, "[%s] battery level: %d %%", this->component_->get_name().c_str(), battery_level);
            publish_sensor(this->component_->battery_level(), battery_level);
            return true;
        }

        static const char *const WRONG_KEY_HINT = "decrypted data is implausible - wrong secret_key?";

        bool TemperatureProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            const char *name = this->component_->get_name().c_str();
            ESP_LOGV(TAG, "[%s] TEMP RAW BLE[%d]: %s", name, value_len, format_hex_pretty(value, value_len).c_str());

            auto t_data = new TemperatureData(this->xxtea_, value, value_len);
            if (!t_data->valid)
            {
                ESP_LOGE(TAG, "[%s] temperature: %s (decrypted %s)", name, WRONG_KEY_HINT, format_hex_pretty(t_data->raw_, t_data->length).c_str());
                delete t_data;
                return false;
            }
            this->data.reset(t_data);
            ESP_LOGV(TAG, "[%s] TEMP DECRYPTED: %s", name, format_hex_pretty(t_data->raw_, t_data->length).c_str());
            ESP_LOGD(TAG, "[%s] TEMP PROCESSED: room=%.1f target=%.1f", name, t_data->room_temperature, t_data->target_temperature);

            publish_sensor(this->component_->temperature(), t_data->room_temperature);

            this->component_->target_temperature = t_data->target_temperature;
            this->component_->current_temperature = t_data->room_temperature;
            this->component_->mark_climate_changed();
            return true;
        }

        bool SettingsProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            const char *name = this->component_->get_name().c_str();
            ESP_LOGV(TAG, "[%s] SETTINGS RAW BLE[%d]: %s", name, value_len, format_hex_pretty(value, value_len).c_str());

            auto s_data = new SettingsData(this->xxtea_, value, value_len);
            if (!s_data->valid)
            {
                // Never keep (and therefore never write back) a settings block that does not look like
                // a real one: writing garbage here is what "resets the valve" (upstream issue #11).
                ESP_LOGE(TAG, "[%s] settings: %s (decrypted %s)", name, WRONG_KEY_HINT, format_hex_pretty(s_data->raw_, s_data->length).c_str());
                delete s_data;
                return false;
            }
            this->data.reset(s_data);

            ESP_LOGV(TAG, "[%s] SETTINGS DECRYPTED: %s", name, format_hex_pretty(s_data->raw_, s_data->length).c_str());
            ESP_LOGD(TAG, "[%s] SETTINGS PROCESSED: mode=%s(%u) min=%.1f max=%.1f frost=%.1f", name, SettingsData::mode_str(s_data->raw_mode), (unsigned)s_data->raw_mode, s_data->temperature_min, s_data->temperature_max, s_data->frost_protection_temperature);
            ESP_LOGV(TAG, "[%s] adaptable_regulation=%d daylight_saving=%d vertical_installation=%d display_flip=%d slow_regulation=%d calibrated=%d valve_installed=%d lock_control=%d", name, s_data->get_adaptable_regulation(), s_data->get_daylight_saving(), s_data->get_vertical_installation(), s_data->get_display_flip(), s_data->get_slow_regulation(), s_data->get_calibrated(), s_data->get_valve_installed(), s_data->get_lock_control());
            ESP_LOGV(TAG, "[%s] vacation: temperature=%.1f from=%d to=%d", name, s_data->vacation_temperature, (int)s_data->vacation_from, (int)s_data->vacation_to);

            auto *c = this->component_.get();
            if (c->device_mode() != nullptr)
                c->device_mode()->publish_state(SettingsData::mode_name(s_data->raw_mode));
            publish_sensor(c->temperature_min(), s_data->temperature_min);
            publish_sensor(c->temperature_max(), s_data->temperature_max);
            publish_sensor(c->frost_protection_temperature(), s_data->frost_protection_temperature);
            publish_sensor(c->vacation_temperature(), s_data->vacation_temperature);
            // no vacation planned: unknown rather than 1970
            publish_sensor(c->vacation_start(), s_data->vacation_from > 0 ? (float)s_data->vacation_from : NAN);
            publish_sensor(c->vacation_end(), s_data->vacation_to > 0 ? (float)s_data->vacation_to : NAN);
            publish_binary(c->child_lock(), s_data->get_lock_control());
            publish_binary(c->valve_installed(), s_data->get_valve_installed());
            publish_binary(c->daylight_saving(), s_data->get_daylight_saving());
            publish_binary(c->adaptive_learning(), s_data->get_adaptable_regulation());
            publish_binary(c->slow_regulation(), s_data->get_slow_regulation());
            publish_binary(c->vertical_installation(), s_data->get_vertical_installation());
            publish_binary(c->display_flip(), s_data->get_display_flip());

            c->mode = s_data->device_mode;
            // Update the visual gauge range from the device-reported min/max (see my_component.h).
            c->set_temperature_range(s_data->temperature_min, s_data->temperature_max);
            c->mark_climate_changed();
            return true;
        }

        bool ErrorsProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            const char *name = this->component_->get_name().c_str();
            auto e_data = new ErrorsData(this->xxtea_, value, value_len);
            if (!e_data->valid)
            {
                ESP_LOGE(TAG, "[%s] errors: could not decrypt", name);
                delete e_data;
                return false;
            }
            this->data.reset(e_data);

            ESP_LOGV(TAG, "[%s] ERRORS DECRYPTED: %s", name, format_hex_pretty(e_data->raw_, e_data->length).c_str());
            ESP_LOGD(TAG, "[%s] errors: flags=%#06x E9 valve=%d E10 time=%d E14 low battery=%d E15 very low battery=%d", name, e_data->flags, e_data->E9_VALVE_DOES_NOT_CLOSE, e_data->E10_INVALID_TIME, e_data->E14_LOW_BATTERY, e_data->E15_VERY_LOW_BATTERY);
            // published by the Device once the whole state (including the clock) has been read
            return true;
        }

        bool TimeProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            const char *name = this->component_->get_name().c_str();
            auto t_data = new TimeData(this->xxtea_, value, value_len);
            if (!t_data->valid)
            {
                ESP_LOGW(TAG, "[%s] time: implausible (decrypted %s)", name, format_hex_pretty(t_data->raw_, t_data->length).c_str());
                delete t_data;
                return false;
            }
            this->data.reset(t_data);
            ESP_LOGD(TAG, "[%s] device clock: utc=%d offset=%d s", name, (int)t_data->epoch, (int)t_data->utc_offset);
            return true;
        }

        bool SecretKeyProperty::init_handle(BLEClient *client)
        {
            if (this->xxtea_->status() != XXTEA_STATUS_NOT_INITIALIZED)
            {
                ESP_LOGV(TAG, "[%s] xxtea is initialized, will not request a read of secret_key", this->component_->get_name().c_str());
                this->handle = INVALID_HANDLE; // never leave a stale handle that a read response could match
                return true;
            }

            auto chr = client->get_characteristic(this->service_uuid, this->characteristic_uuid);
            if (chr != nullptr)
            {
                this->handle = chr->handle;
                return true;
            }

            ESP_LOGW(TAG, "[%s] Danfoss Eco hardware button was not pressed, unable to read the secret key", this->component_->get_name().c_str());
            this->handle = INVALID_HANDLE;
            return false;
        }

        bool SecretKeyProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            if (value_len != SECRET_KEY_LENGTH)
            {
                ESP_LOGE(TAG, "[%s] Unexpected secret_key length: %d", this->component_->get_name().c_str(), value_len);
                return false;
            }

            char key_str[SECRET_KEY_LENGTH * 2 + 1];
            encode_hex(value, value_len, key_str);

            ESP_LOGI(TAG, "[%s] Consider adding below line to your danfoss_eco config:", this->component_->get_name().c_str());
            ESP_LOGI(TAG, "[%s] secret_key: %s", this->component_->get_name().c_str(), key_str);
            this->component_->set_secret_key(value, true);
            return true;
        }

        bool InfoProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            // plain UTF-8 text, possibly NUL padded
            string text;
            for (uint16_t i = 0; i < value_len && value[i] != 0; i++)
                text.push_back((value[i] < 0x20 || value[i] == 0x7F) ? ' ' : (char)value[i]);
            text = sanitize_utf8(text);
            const size_t first = text.find_first_not_of(' ');
            const size_t last = text.find_last_not_of(' ');
            this->value_ = first == string::npos ? string() : text.substr(first, last - first + 1);
            ESP_LOGD(TAG, "[%s] %s: %s", this->component_->get_name().c_str(), this->label_, this->value_.c_str());
            auto *s = this->component_->info_sensor(this->field_);
            if (s != nullptr)
                s->publish_state(this->value_);
            return true;
        }

        bool NameProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            auto n_data = new NameData(this->xxtea_, value, value_len);
            if (!n_data->valid)
            {
                ESP_LOGW(TAG, "[%s] device name: could not decrypt (%u bytes)", this->component_->get_name().c_str(), (unsigned)value_len);
                delete n_data;
                return false;
            }
            ESP_LOGD(TAG, "[%s] device name: '%s'", this->component_->get_name().c_str(), n_data->name.c_str());
            if (this->component_->thermostat_name() != nullptr)
                this->component_->thermostat_name()->publish_state(n_data->name);
            this->data.reset(n_data);
            return true;
        }

        bool PinSettingsProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            // never log this payload: it carries the PIN
            auto p_data = new PinSettingsData(this->xxtea_, value, value_len);
            if (!p_data->valid)
            {
                ESP_LOGW(TAG, "[%s] PIN settings: could not decrypt", this->component_->get_name().c_str());
                delete p_data;
                return false;
            }
            ESP_LOGD(TAG, "[%s] PIN protection: %s", this->component_->get_name().c_str(), p_data->pin_enabled ? "on" : "off");
            publish_binary(this->component_->pin_protection(), p_data->pin_enabled);
            this->data.reset(p_data);
            return true;
        }

        bool ScheduleProperty::update_state(uint8_t *value, uint16_t value_len)
        {
            auto s_data = new SchedulePartData(this->xxtea_, this->part_, value, value_len);
            if (!s_data->valid)
            {
                ESP_LOGW(TAG, "[%s] schedule part %u: implausible (decrypted %s)", this->component_->get_name().c_str(), (unsigned)this->part_ + 1, format_hex_pretty(s_data->raw_, s_data->length).c_str());
                delete s_data;
                return false;
            }
            ESP_LOGV(TAG, "[%s] SCHEDULE %u DECRYPTED: %s", this->component_->get_name().c_str(), (unsigned)this->part_ + 1, format_hex_pretty(s_data->raw_, s_data->length).c_str());
            this->data.reset(s_data);
            return true;
        }

    } // namespace danfoss_eco
} // namespace esphome

#endif // USE_ESP32
