#include "device.h"
#include <cmath>

#ifdef USE_ESP32

namespace esphome
{
  namespace danfoss_eco
  {
    void Device::setup()
    {
      shared_ptr<MyComponent> sp_this(this);

      this->p_pin = make_shared<WritableProperty>(sp_this, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_PIN);
      this->p_battery = make_shared<BatteryProperty>(sp_this, xxtea);
      this->p_temperature = make_shared<TemperatureProperty>(sp_this, xxtea);
      this->p_settings = make_shared<SettingsProperty>(sp_this, xxtea);
      this->p_errors = make_shared<ErrorsProperty>(sp_this, xxtea);
      this->p_secret_key = make_shared<SecretKeyProperty>(sp_this, xxtea);

      this->properties = {this->p_pin, this->p_battery, this->p_temperature, this->p_settings, this->p_errors, this->p_secret_key};
      // pretend, we have already discovered the device
      copy_address(this->parent()->get_address(), this->parent()->get_remote_bda());
      // NOTE: the old explicit `this->parent()->set_state(ClientState::INIT)` was removed: INIT is
      // already the default BLEClient state in modern ESPHome, and forcing it here interfered with
      // the reworked connect() flow below.
    }

    void Device::loop()
    {
      if (this->status_has_error())
      {
        this->disconnect();
        this->status_clear_error();
      }

      if (this->node_state != ClientState::ESTABLISHED)
        return;

      Command *cmd = this->commands_.pop();
      while (cmd != nullptr)
      {
        if (cmd->execute(this->parent()))
          this->request_counter_++;

        delete cmd;
        cmd = this->commands_.pop();
      }

      // once we are done with pending commands - check to see if there are any pending requests
      // if there are no pending requests - we are done with the device for now and should disconnect
      if (this->request_counter_ == 0)
        this->disconnect();
    }

    void Device::update()
    {
      this->connect();

      if (this->xxtea->status() == XXTEA_STATUS_SUCCESS)
      {
        ESP_LOGI(TAG, "[%s] requesting device state", this->get_name().c_str());

        this->commands_.push(new Command(CommandType::READ, this->p_battery));
        this->commands_.push(new Command(CommandType::READ, this->p_temperature));
        this->commands_.push(new Command(CommandType::READ, this->p_settings));
        this->commands_.push(new Command(CommandType::READ, this->p_errors));
      }
    }

    void Device::control(const ClimateCall &call)
    {
      // SAFETY: never write both the mode (16-byte settings characteristic) and the target
      // temperature (8-byte temperature characteristic) within the same connection cycle.
      // Doing so was observed to corrupt the eTRV (switching heating<->idle in Home Assistant could
      // leave the thermostat malfunctioning until a battery removal + re-init). When Home Assistant
      // sends a combined call, we apply the MODE only and IGNORE the temperature part; the
      // temperature can simply be re-sent as a separate call.
      if (call.get_mode().has_value() && call.get_target_temperature().has_value())
      {
        ESP_LOGW(TAG, "[%s] combined mode+temperature call: applying mode only, temperature ignored (please re-send temperature separately)", this->get_name().c_str());
        ClimateCall mode_call(this);
        mode_call.set_mode(*call.get_mode());
        this->control(mode_call);
        return;
      }

      if (call.get_target_temperature().has_value())
      {
        if (!this->p_temperature->data)
        {
          ESP_LOGE(TAG, "[%s] no temperature data yet - device must be read before it can be controlled", this->get_name().c_str());
          return;
        }

        TemperatureData &t_data = (TemperatureData &)(*this->p_temperature->data);
        float new_temp = *call.get_target_temperature();

        // The eTRV only accepts target temperatures in its physical range (5-30 C). Reject anything
        // outside to avoid writing garbage to the device.
        if (new_temp < 5.0f || new_temp > 30.0f)
        {
          ESP_LOGE(TAG, "[%s] rejecting out-of-range target temperature: %.1f C (allowed 5.0-30.0)", this->get_name().c_str(), new_temp);
          return;
        }

        // Only write when the value actually changed (avoids needless BLE round-trips / battery drain).
        if (std::abs(t_data.target_temperature - new_temp) >= 0.1f)
        {
          ESP_LOGD(TAG, "[%s] target temperature change: %.1f -> %.1f C", this->get_name().c_str(), t_data.target_temperature, new_temp);
          t_data.target_temperature = new_temp;
          this->commands_.push(new Command(CommandType::WRITE, this->p_temperature));
          this->connect();
        }
        else
        {
          ESP_LOGD(TAG, "[%s] target temperature unchanged (%.1f C), skipping write", this->get_name().c_str(), new_temp);
        }
      }

      if (call.get_mode().has_value())
      {
        if (!this->p_settings->data)
        {
          ESP_LOGE(TAG, "[%s] no settings data yet - device must be read before its mode can be changed", this->get_name().c_str());
          return;
        }

        SettingsData &s_data = (SettingsData &)(*this->p_settings->data);
        ClimateMode new_mode = *call.get_mode();
        ClimateMode current_mode = s_data.device_mode;

        if (new_mode != current_mode)
        {
          ESP_LOGD(TAG, "[%s] mode change: %d -> %d", this->get_name().c_str(), (int)current_mode, (int)new_mode);

          s_data.device_mode = new_mode;
          // update state immediately to avoid delays in HA UI
          this->mode = s_data.device_mode;
          this->publish_state();
          this->commands_.push(new Command(CommandType::WRITE, this->p_settings));
          this->connect();
        }
        else
        {
          ESP_LOGD(TAG, "[%s] mode unchanged (%d), skipping write", this->get_name().c_str(), (int)current_mode);
        }
      }
    }

    void Device::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
    {
      switch (event)
      {
      case ESP_GATTC_CONNECT_EVT:
        if (memcmp(param->connect.remote_bda, this->parent()->get_remote_bda(), 6) != 0)
          return; // event does not belong to this client, exit gattc_event_handler

        ESP_LOGD(TAG, "[%s] connect, conn_id=%d", this->get_name().c_str(), param->connect.conn_id);
        break;

      case ESP_GATTC_OPEN_EVT:
        if (param->open.status == ESP_GATT_OK)
          ESP_LOGV(TAG, "[%s] open, conn_id=%d", this->get_name().c_str(), param->open.conn_id);
        else
          // A failed open (e.g. transient ESP_GATT_ERROR 0x85) is only logged here. Do NOT call
          // disconnect()/set_enabled(false): the ble_client must stay enabled so that auto_connect
          // (the scanner's parse_device path) can re-establish the connection once the eTRV advertises
          // again. Disabling it here permanently parks the client - with a long update_interval the
          // device would never reconnect. (This reverts an earlier "clean retry" attempt that broke
          // recovery.)
          ESP_LOGW(TAG, "[%s] failed to open, conn_id=%d, status=%#04x", this->get_name().c_str(), param->open.conn_id, param->open.status);
        break;

      case ESP_GATTC_CLOSE_EVT:
        if (param->close.status == ESP_GATT_OK)
          ESP_LOGV(TAG, "[%s] close, conn_id=%d, reason=%d", this->get_name().c_str(), param->close.conn_id, param->close.reason);
        else
          ESP_LOGW(TAG, "[%s] failed to close, conn_id=%d, status=%#04x", this->get_name().c_str(), param->close.conn_id, param->close.status);
        break;

      case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGD(TAG, "[%s] disconnect, conn_id=%d, reason=%#04x", this->get_name().c_str(), param->disconnect.conn_id, (int)param->disconnect.reason);
        break;

      case ESP_GATTC_SEARCH_CMPL_EVT:
        for (auto p : this->properties)
          p->init_handle(this->parent());

        write_pin();
        break;

      case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.handle == this->p_pin->handle)
          this->on_write_pin(param->write);
        else
          this->on_write(param->write);
        break;

      case ESP_GATTC_READ_CHAR_EVT:
        this->on_read(param->read);
        break;

      default:
        ESP_LOGV(TAG, "[%s] unhandled event: event=%d, gattc_if=%d", this->get_name().c_str(), (int)event, gattc_if);
        break;
      }
    }

    void Device::write_pin()
    {
      ESP_LOGD(TAG, "[%s] writing pin", this->get_name().c_str());

      uint8_t pin_bytes[sizeof(uint32_t)];
      write_int(pin_bytes, 0, this->pin_code_);

      if (!this->p_pin->write_request(this->parent(), pin_bytes, sizeof(pin_bytes)))
        this->status_set_error();
    }

    void Device::on_read(esp_ble_gattc_cb_param_t::gattc_read_char_evt_param param)
    {
      this->request_counter_--;
      if (param.status != ESP_GATT_OK)
      {
        ESP_LOGW(TAG, "[%s] failed to read characteristic: handle=%#04x, status=%#04x", this->get_name().c_str(), param.handle, param.status);
        return;
      }

      auto device_property = find_if(properties.begin(), properties.end(),
                                     [&param](shared_ptr<DeviceProperty> p)
                                     { return p->handle == param.handle; });

      if (device_property != properties.end())
        (*device_property)->update_state(param.value, param.value_len);
      else
        ESP_LOGW(TAG, "[%s] unknown property with handle=%#04x", this->get_name().c_str(), param.handle);
    }

    void Device::on_write(esp_ble_gattc_cb_param_t::gattc_write_evt_param param)
    {
      this->request_counter_--;
      if (param.status != ESP_GATT_OK)
        ESP_LOGW(TAG, "[%s] failed to write characteristic: handle=%#04x, status=%#04x", this->get_name().c_str(), param.handle, param.status);
      else
        update();
    }

    void Device::on_write_pin(esp_ble_gattc_cb_param_t::gattc_write_evt_param param)
    {
      if (param.status != ESP_GATT_OK)
      {
        ESP_LOGE(TAG, "[%s] pin FAILED, status=%#04x", this->get_name().c_str(), param.status);
        this->disconnect();
        this->mark_failed();
        return;
      }

      ESP_LOGD(TAG, "[%s] pin OK", this->get_name().c_str());
      this->node_state = ClientState::ESTABLISHED;

      // after PIN is written, we might need to read the secret_key from the device
      if (this->xxtea->status() == XXTEA_STATUS_NOT_INITIALIZED && this->p_secret_key->handle != INVALID_HANDLE)
      {
        ESP_LOGD(TAG, "[%s] attempting to read the device secret_key", this->get_name().c_str());
        this->commands_.push(new Command(CommandType::READ, this->p_secret_key));
      }
    }

    void Device::connect()
    {
      if (this->node_state == ClientState::ESTABLISHED)
      {
        return;
      }

      if (this->xxtea->status() == XXTEA_STATUS_NOT_INITIALIZED)
        ESP_LOGI(TAG, "[%s] Short press Danfoss Eco hardware button NOW in order to allow reading the secret key", this->get_name().c_str());

      if (!parent()->enabled)
      {
        ESP_LOGD(TAG, "[%s] re-enabling ble_client", this->get_name().c_str());
        parent()->set_enabled(true);
      }

      // NOTE: the original code manually called esp_ble_gap_stop_scanning() and
      // set_state(ClientState::READY_TO_CONNECT). Both are gone in modern ESPHome:
      //  - ClientState::READY_TO_CONNECT no longer exists; BLEClientBase::connect() is the API.
      //  - The ESP32BLETracker now pauses scanning automatically whenever a client is in the
      //    transient CONNECTING/DISCOVERED/DISCONNECTING states (see esp32_ble_tracker), so a manual
      //    esp_ble_gap_stop_scanning() is redundant. Worse, the raw GAP call bypasses the tracker's
      //    scanner state machine and was found to prevent a *second* eTRV from connecting.
      ESP_LOGD(TAG, "[%s] initiating BLE connection", this->get_name().c_str());
      this->parent()->connect(); // trigger BLE connection attempt
    }

    void Device::disconnect()
    {
      this->parent()->set_enabled(false);
      this->node_state = ClientState::IDLE;
    }

    void Device::set_pin_code(const string &str)
    {
      if (str.length() > 0)
        this->pin_code_ = atoi((const char *)str.c_str());

      ESP_LOGD(TAG, "[%s] PIN: %04d", this->get_name().c_str(), this->pin_code_);
    }

    void Device::set_secret_key(const string &str)
    {
      // initialize the preference object
      uint32_t hash = fnv1_hash("danfoss_eco_secret__" + this->get_name());
      this->secret_pref_ = global_preferences->make_preference<SecretKeyValue>(hash, true);

      if (str.length() > 0)
      {
        uint8_t buff[SECRET_KEY_LENGTH];
        ESP_LOGD(TAG, "[%s] secret_key was passed via config", this->get_name().c_str());
        parse_hex_str(str.c_str(), 32, buff);
        this->set_secret_key(buff, false);
      }
      else
      {
        auto key_buff = SecretKeyValue();
        if (this->secret_pref_.load(&key_buff))
        {
          // use persisted secret value
          ESP_LOGD(TAG, "[%s] secret_key was loaded from flash", this->get_name().c_str());
          this->set_secret_key(key_buff.value, false);
        }
      }
    }

    void Device::set_secret_key(uint8_t *key, bool persist)
    {
      ESP_LOGD(TAG, "[%s] secret_key bytes: %s", this->get_name().c_str(), format_hex_pretty(key, SECRET_KEY_LENGTH).c_str());

      int status = this->xxtea->set_key(key, SECRET_KEY_LENGTH);
      if (status != XXTEA_STATUS_SUCCESS)
      {
        ESP_LOGE(TAG, "xxtea initialization failed, status: %d", status);
        this->mark_failed();
      }
      else if (persist)
      {
        // if xxtea was initialized successfully and secret_key should be persisted
        auto key_buff = SecretKeyValue(key);
        this->secret_pref_.save(&key_buff);
        global_preferences->sync();

        ESP_LOGI(TAG, "[%s] secret_key was saved to flash", this->get_name().c_str());
      }
    }

  } // namespace danfoss_eco
} // namespace esphome

#endif
