#include "device.h"
#include "esphome/core/hal.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#ifdef USE_TIME_TIMEZONE
#include "esphome/components/time/posix_tz.h"
#endif

#ifdef USE_ESP32

namespace esphome
{
  namespace danfoss_eco
  {
    // Any link older than this is torn down, no matter what the ESPHome client machinery thinks.
    // A complete transaction (PIN, reads, a write, re-read) takes ~2.5 s with known handles and
    // ~10 s with service discovery at the eTRV's slow connection interval.
    static constexpr uint32_t LINK_WATCHDOG_MS = 60000;
    // The ESPHome client must never stay outside IDLE without a link of ours for longer than this
    // (Bluedroid bounds an open to 20 s and ESPHome a disconnect to 10 s, so this only fires on a
    // genuine anomaly).
    static constexpr uint32_t CLIENT_WATCHDOG_MS = 90000;
    // If the tracker has not promoted our DISCOVERED request within this time *while no other
    // Danfoss client is connecting* (the tracker promotes one client at a time, so waiting behind a
    // sibling is normal), fall back to a direct BLEClientBase::connect().
    static constexpr uint32_t PROMOTION_FALLBACK_MS = 15000;
    // Pause before the follow-up link of a transaction that needs two links (mode, then set point).
    static constexpr uint32_t RECONNECT_DELAY_MS = 3000;
    // Fast phase back-off (index = failures in this fast phase): 3 s, 15 s, 30 s, 60 s, then 120 s.
    // A failed open (the eTRV was not heard for 20 s) starts at 15 s.
    static constexpr uint32_t RETRY_BACKOFF_MS[] = {3000, 15000, 30000, 60000, 120000};
    // Slow phase (after retry_window): every 5 min, after one more hour every 15 min. A failed open
    // costs the eTRV nothing but blocks the other thermostats for 20 s: 6.7 % resp. 2.2 % airtime.
    static constexpr uint32_t SLOW_RETRY_MS = 300000;
    static constexpr uint32_t SLOW_RETRY_LONG_MS = 900000;
    static constexpr uint32_t SLOW_LONG_AFTER_MS = 3600000;
    // A requested set point / mode that could not be delivered for this long is dropped (and the
    // device's real values are shown again).
    static constexpr uint32_t COMMAND_TTL_MS = 24UL * 3600UL * 1000UL;
    // A request resumed after a restart (its original age is unknown) gets this long.
    static constexpr uint32_t RESUME_TTL_MS = 3600000;
    // A new command re-enters the fast retry phase at most this often (an automation re-sending a
    // value every minute must not keep an unreachable eTRV in the fast phase forever).
    static constexpr uint32_t FAST_REARM_MIN_MS = 600000;
    // Protocol errors that retrying cannot fix (PIN rejected, undecryptable data = wrong secret_key,
    // missing characteristics): give up after this many in a row. Reset by a successful read.
    static constexpr uint8_t MAX_HARD_ERRORS = 3;
    // Clock sync (optional, needs `time_id`): write the time when the eTRV clock is off by more
    // than this (or holds another offset than expected).
    static constexpr int32_t TIME_DRIFT_MAX_S = 120;
    // Never write the clock more often than this, whatever the eTRV reports back (a unit that
    // ignores or rejects the write must not cause a write/re-read loop).
    static constexpr uint32_t TIME_SYNC_MIN_INTERVAL_MS = 3600000;
    // A clock that was set and is later found off by more than this has been lost by the eTRV
    // (reported as a problem: the eTRV does not raise E10 for it).
    static constexpr int32_t CLOCK_LOST_S = 3600;
    // E10 ("invalid clock information", raised after a battery change) is latched: the eTRV never
    // clears it, the Danfoss app acknowledges it by writing the error flags back without it. We do
    // the same once the clock is right - at most this often (a unit that keeps raising it again
    // must not cause a write loop).
    static constexpr uint32_t E10_ACK_MIN_INTERVAL_MS = 3600000;
    // The weekly schedule can only be changed with the Danfoss app: re-read it once a day.
    static constexpr uint32_t SCHEDULE_REFRESH_MS = 24UL * 3600UL * 1000UL;
    // The first response on a link (the PIN write) can take >10 s: at a poor signal Bluedroid may
    // still be running its own service discovery before it sends queued requests (measured: median
    // 2.0 s, p99 10.7 s, max 12.6 s). The link watchdog still bounds the whole link.
    static constexpr uint32_t PIN_TIMEOUT_MS = 30000;
    // A command equal to a device value read this recently is not written again.
    static constexpr uint32_t FRESH_READ_MS = 60000;
    // Diagnostics: a one-line link/transaction summary per device at VERBOSE every minute.
    static constexpr uint32_t STATUS_LOG_INTERVAL_MS = 60000;
    static constexpr uint16_t NO_CONN_ID = 0xFFFF;

    static constexpr uint8_t BATCH_NONE = 0, BATCH_SECRET_KEY = 1, BATCH_READ = 2, BATCH_WRITE_SETTINGS = 3, BATCH_WRITE_TEMPERATURE = 4, BATCH_WRITE_TIME = 5, BATCH_WRITE_ERRORS = 6, BATCH_INFO = 7;

    // Informational reads (bit masks for Device::info_pending_): Device Information Service strings
    // (bits 0..5 = InfoField), the device name, the PIN protection flag and the three schedule parts.
    static constexpr uint16_t INFO_BIT_NAME = 1u << 6;
    static constexpr uint16_t INFO_BIT_PIN = 1u << 7;
    static constexpr uint16_t INFO_BITS_SCHEDULE = 7u << 8;

    static const char *batch_str(uint8_t b)
    {
      switch (b)
      {
      case BATCH_SECRET_KEY:
        return "SECRET_KEY";
      case BATCH_READ:
        return "READ";
      case BATCH_WRITE_SETTINGS:
        return "WRITE_SETTINGS";
      case BATCH_WRITE_TEMPERATURE:
        return "WRITE_TEMPERATURE";
      case BATCH_WRITE_TIME:
        return "WRITE_TIME";
      case BATCH_WRITE_ERRORS:
        return "WRITE_ERRORS";
      case BATCH_INFO:
        return "INFO";
      default:
        return "NONE";
      }
    }

    static const char *client_state_str(ClientState st)
    {
      switch (st)
      {
      case ClientState::INIT:
        return "INIT";
      case ClientState::DISCONNECTING:
        return "DISCONNECTING";
      case ClientState::IDLE:
        return "IDLE";
      case ClientState::DISCOVERED:
        return "DISCOVERED";
      case ClientState::CONNECTING:
        return "CONNECTING";
      case ClientState::CONNECTED:
        return "CONNECTED";
      case ClientState::ESTABLISHED:
        return "ESTABLISHED";
      default:
        return "?";
      }
    }

    // Bluedroid reports link/stack problems with the 0x80..0x8f status range (ESP_GATT_ERROR 0x85 =
    // the link died underneath the request, BUSY, CONGESTED, NO_RESOURCES ...). Those are retried on
    // a new link. Anything else is the eTRV rejecting the request.
    static bool is_transient_status(esp_gatt_status_t status) { return status >= 0x80 && status <= 0x8f; }

    // All Danfoss devices on this ESP (the tracker serialises their connection attempts).
    static std::vector<Device *> &all_devices()
    {
      static std::vector<Device *> devices;
      return devices;
    }

    bool Device::sibling_connecting_() const
    {
      for (auto *d : all_devices())
      {
        if (d != this && d->parent()->state() == ClientState::CONNECTING)
          return true;
      }
      return false;
    }

    uint32_t Device::link_age_ms_() const
    {
      return this->link_up_ ? (millis() - this->link_started_ms_) : 0;
    }

    bool Device::time_sync_enabled_() const
    {
#ifdef USE_TIME
      return this->time_ != nullptr;
#else
      return false;
#endif
    }

    // One-line link/transaction summary (diagnostics). level: ESPHOME_LOG_LEVEL_ERROR/WARN/DEBUG/VERBOSE.
    void Device::log_link_(const char *where, int level)
    {
#define DANFOSS_LINK_FMT "[%s] %s: parent=%s node=%s enabled=%d conn_id=%d link_up=%d age=%u ms inflight=%u batch=%s pending[read=%d temp=%d settings=%d key=%d time=%d e10=%d info=%#05x] want_link=%d slow=%d links=%u opens_failed=%u requests=%u hard_errors=%u"
#define DANFOSS_LINK_ARGS this->get_name().c_str(), where, client_state_str(this->parent()->state()), client_state_str(this->node_state), (int)this->parent()->enabled, (int)this->parent()->get_conn_id(), (int)this->link_up_, (unsigned)this->link_age_ms_(), (unsigned)this->inflight_, batch_str(this->batch_), (int)this->pending_read_, (int)this->pending_write_temperature_, (int)this->pending_write_settings_, (int)this->pending_secret_key_, (int)this->pending_time_sync_, (int)this->pending_e10_ack_, (unsigned)this->info_pending_, (int)this->want_link_, (int)this->slow_phase_, (unsigned)this->links_this_txn_, (unsigned)this->open_failures_, (unsigned)this->connect_attempts_, (unsigned)this->consecutive_hard_errors_
      switch (level)
      {
      case ESPHOME_LOG_LEVEL_ERROR:
        ESP_LOGE(TAG, DANFOSS_LINK_FMT, DANFOSS_LINK_ARGS);
        break;
      case ESPHOME_LOG_LEVEL_WARN:
        ESP_LOGW(TAG, DANFOSS_LINK_FMT, DANFOSS_LINK_ARGS);
        break;
      case ESPHOME_LOG_LEVEL_DEBUG:
        ESP_LOGD(TAG, DANFOSS_LINK_FMT, DANFOSS_LINK_ARGS);
        break;
      default:
        ESP_LOGV(TAG, DANFOSS_LINK_FMT, DANFOSS_LINK_ARGS);
        break;
      }
#undef DANFOSS_LINK_FMT
#undef DANFOSS_LINK_ARGS
    }

    void Device::dump_config()
    {
      LOG_CLIMATE("", "Danfoss Eco eTRV", this);
      // BLEClientBase::address_str() returns `const char *` in ESPHome 2026.x (was std::string).
      ESP_LOGCONFIG(TAG, "  MAC Address: %s", this->parent()->address_str());
      ESP_LOGCONFIG(TAG, "  Request timeout: %u ms", (unsigned)this->request_timeout_ms_);
      ESP_LOGCONFIG(TAG, "  Fast retry phase: %u ms, then every %u / %u s", (unsigned)this->retry_window_ms_, (unsigned)(SLOW_RETRY_MS / 1000), (unsigned)(SLOW_RETRY_LONG_MS / 1000));
      ESP_LOGCONFIG(TAG, "  Link watchdog: %u ms", (unsigned)LINK_WATCHDOG_MS);
      ESP_LOGCONFIG(TAG, "  Clock sync: %s", this->time_sync_enabled_() ? "enabled" : "disabled (no time_id)");
      LOG_SENSOR("", "Battery Level", this->battery_level_);
      LOG_SENSOR("", "Room Temperature", this->temperature_);
      LOG_BINARY_SENSOR("", "Problems", this->problems_);
      LOG_TEXT_SENSOR("", "Problems (Detail)", this->problems_detail_);
      LOG_BINARY_SENSOR("", "Connection", this->connection_);
      LOG_BINARY_SENSOR("", "Low Battery", this->low_battery_);
      LOG_BINARY_SENSOR("", "Valve Error", this->valve_error_);
      LOG_BINARY_SENSOR("", "Motor Error", this->motor_error_);
      LOG_BINARY_SENSOR("", "Clock Error", this->clock_error_);
      LOG_BINARY_SENSOR("", "Hardware Error", this->hardware_error_);
      LOG_TEXT_SENSOR("", "Device Mode", this->device_mode_);
      LOG_SENSOR("", "Temperature Min", this->temperature_min_);
      LOG_SENSOR("", "Temperature Max", this->temperature_max_);
      LOG_SENSOR("", "Frost Protection Temperature", this->frost_protection_temperature_);
      LOG_SENSOR("", "Vacation Temperature", this->vacation_temperature_);
      LOG_SENSOR("", "Vacation Start", this->vacation_start_);
      LOG_SENSOR("", "Vacation End", this->vacation_end_);
      LOG_BINARY_SENSOR("", "Child Lock", this->child_lock_);
      LOG_BINARY_SENSOR("", "Valve Installed", this->valve_installed_);
      LOG_BINARY_SENSOR("", "Daylight Saving", this->daylight_saving_);
      LOG_BINARY_SENSOR("", "Adaptive Learning", this->adaptive_learning_);
      LOG_BINARY_SENSOR("", "Slow Regulation", this->slow_regulation_);
      LOG_BINARY_SENSOR("", "Vertical Installation", this->vertical_installation_);
      LOG_BINARY_SENSOR("", "Display Flip", this->display_flip_);
      LOG_BINARY_SENSOR("", "PIN Protection", this->pin_protection_);
      LOG_TEXT_SENSOR("", "Thermostat Name", this->thermostat_name_);
      LOG_TEXT_SENSOR("", "Schedule", this->schedule_);
      LOG_SENSOR("", "Schedule Home Temperature", this->schedule_home_temperature_);
      LOG_SENSOR("", "Schedule Away Temperature", this->schedule_away_temperature_);
      LOG_TEXT_SENSOR("", "Manufacturer", this->info_sensors_[INFO_MANUFACTURER]);
      LOG_TEXT_SENSOR("", "Model", this->info_sensors_[INFO_MODEL]);
      LOG_TEXT_SENSOR("", "Serial Number", this->info_sensors_[INFO_SERIAL]);
      LOG_TEXT_SENSOR("", "Hardware Version", this->info_sensors_[INFO_HARDWARE]);
      LOG_TEXT_SENSOR("", "Firmware Version", this->info_sensors_[INFO_FIRMWARE]);
      LOG_TEXT_SENSOR("", "Software Version", this->info_sensors_[INFO_SOFTWARE]);
      if (this->p_info[INFO_FIRMWARE] != nullptr && !this->p_info[INFO_FIRMWARE]->value().empty())
        ESP_LOGCONFIG(TAG, "  eTRV firmware: %s", this->p_info[INFO_FIRMWARE]->value().c_str());
    }

    void Device::setup()
    {
      shared_ptr<MyComponent> sp_this(this);

      this->p_pin = make_shared<WritableProperty>(sp_this, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_PIN);
      this->p_battery = make_shared<BatteryProperty>(sp_this, xxtea);
      this->p_temperature = make_shared<TemperatureProperty>(sp_this, xxtea);
      this->p_settings = make_shared<SettingsProperty>(sp_this, xxtea);
      this->p_errors = make_shared<ErrorsProperty>(sp_this, xxtea);
      this->p_time = make_shared<TimeProperty>(sp_this, xxtea);
      this->p_secret_key = make_shared<SecretKeyProperty>(sp_this, xxtea);
      this->p_pin_settings = make_shared<PinSettingsProperty>(sp_this, xxtea);
      this->p_name = make_shared<NameProperty>(sp_this, xxtea);
      for (uint8_t part = 0; part < 3; part++)
        this->p_schedule[part] = make_shared<ScheduleProperty>(sp_this, xxtea, part);
      this->p_info[INFO_MANUFACTURER] = make_shared<InfoProperty>(sp_this, xxtea, 0x2A29, INFO_MANUFACTURER, "manufacturer");
      this->p_info[INFO_MODEL] = make_shared<InfoProperty>(sp_this, xxtea, 0x2A24, INFO_MODEL, "model");
      this->p_info[INFO_SERIAL] = make_shared<InfoProperty>(sp_this, xxtea, 0x2A25, INFO_SERIAL, "serial number");
      this->p_info[INFO_HARDWARE] = make_shared<InfoProperty>(sp_this, xxtea, 0x2A27, INFO_HARDWARE, "hardware revision");
      this->p_info[INFO_FIRMWARE] = make_shared<InfoProperty>(sp_this, xxtea, 0x2A26, INFO_FIRMWARE, "firmware revision");
      this->p_info[INFO_SOFTWARE] = make_shared<InfoProperty>(sp_this, xxtea, 0x2A28, INFO_SOFTWARE, "software revision");

      this->properties = {this->p_pin, this->p_battery, this->p_temperature, this->p_settings, this->p_errors, this->p_time, this->p_secret_key, this->p_pin_settings, this->p_name};
      for (auto &p : this->p_schedule)
        this->properties.insert(p);
      for (auto &p : this->p_info)
        this->properties.insert(p);
      // Informational values are read once after boot, on the first link that has done its work.
      this->info_pending_ = this->info_wanted_mask_();
      // pretend, we have already discovered the device
      copy_address(this->parent()->get_address(), this->parent()->get_remote_bda());
      all_devices().push_back(this);

      // Show the last known state (not "off / unknown") until the first read after boot.
      auto restore = this->restore_state_();
      if (restore.has_value())
        restore->apply(this);

      // Requests that were not delivered before the restart (keyed by the eTRV's MAC address).
      this->pending_pref_ = global_preferences->make_preference<PendingWrites>(fnv1_hash(std::string("danfoss_eco_pending__") + this->parent()->address_str()));
      this->load_pending_();

      if (restore.has_value() || this->has_writes_())
        this->publish_climate_state();
      if (this->has_writes_())
        this->request_work_(true, true);
    }

    // ------------------------------------------------------------------------------------------
    // main loop: watchdogs + transaction driver
    // ------------------------------------------------------------------------------------------
    void Device::loop()
    {
      const uint32_t now = millis();

      // Diagnostics: BLEClient::set_state() silently overwrites node_state on every parent state
      // change, so log the transitions to get a complete timeline.
      if (this->node_state != this->last_node_state_)
      {
        ESP_LOGV(TAG, "[%s] node_state %s -> %s (parent=%s link_age=%u ms inflight=%u)", this->get_name().c_str(), client_state_str(this->last_node_state_), client_state_str(this->node_state), client_state_str(this->parent()->state()), (unsigned)this->link_age_ms_(), (unsigned)this->inflight_);
        this->last_node_state_ = this->node_state;
      }
      if (now - this->last_status_log_ms_ >= STATUS_LOG_INTERVAL_MS)
      {
        this->last_status_log_ms_ = now;
        this->log_link_("status", ESPHOME_LOG_LEVEL_VERBOSE);
      }

      // The ble_client starts life *enabled* with auto_connect, i.e. the tracker would open a
      // link to the eTRV as soon as it is heard, even before our first poll. Disable it once the
      // client has registered (IDLE); the first update() re-enables it a few seconds later.
      if (!this->boot_disable_done_ && this->parent()->state() == ClientState::IDLE)
      {
        this->boot_disable_done_ = true;
        if (!this->want_link_ && this->parent()->enabled)
        {
          ESP_LOGD(TAG, "[%s] boot: parking ble_client until the first poll", this->get_name().c_str());
          this->parent()->set_enabled(false);
        }
      }

      const auto pst = this->parent()->state();
      // Restart the promotion-fallback timer on every entry into DISCOVERED (ours or auto_connect's).
      if (pst == ClientState::DISCOVERED && this->last_parent_state_ != ClientState::DISCOVERED)
        this->connect_requested_ms_ = now;
      this->last_parent_state_ = pst;

      // --- safety net: ESPHome client stuck outside IDLE without a link of ours ---
      const bool client_busy = !this->link_up_ && (pst == ClientState::CONNECTING || pst == ClientState::CONNECTED || pst == ClientState::ESTABLISHED || pst == ClientState::DISCONNECTING);
      if (!client_busy)
      {
        this->parent_busy_since_ms_ = 0;
      }
      else if (this->parent_busy_since_ms_ == 0)
      {
        this->parent_busy_since_ms_ = now | 1;
      }
      else if (now - this->parent_busy_since_ms_ > CLIENT_WATCHDOG_MS)
      {
        this->parent_busy_since_ms_ = 0;
        this->log_link_("CLIENT WATCHDOG: BLE client stuck without a link, resetting it", ESPHOME_LOG_LEVEL_ERROR);
        // ESP_GATT_ALREADY_OPEN is accepted by ESPHome as an open without a CONNECT_EVT, so ESPHome
        // has no conn_id to close: close it with the conn_id the OPEN_EVT carried.
        if (this->last_open_status_ == ESP_GATT_ALREADY_OPEN && this->last_open_conn_id_ != NO_CONN_ID && this->parent()->get_conn_id() == NO_CONN_ID)
          esp_ble_gattc_close(this->parent()->get_gattc_if(), this->last_open_conn_id_);
        if (this->parent()->state() == ClientState::CONNECTING && this->parent()->get_conn_id() != NO_CONN_ID)
          this->parent()->unconditional_disconnect(); // OPEN_EVT lost: close by conn_id whatever the state
        else
          this->parent()->disconnect();
        if (this->parent()->get_conn_id() == NO_CONN_ID && this->parent()->state() != ClientState::IDLE)
          this->parent()->set_state(ClientState::IDLE);
        this->link_failed_("client watchdog");
        return;
      }

      // --- safety net 1: link watchdog ---
      if (this->link_up_ && (now - this->link_started_ms_) > LINK_WATCHDOG_MS)
      {
        this->log_link_("WATCHDOG: link open too long, forcing disconnect", ESPHOME_LOG_LEVEL_ERROR);
        this->link_failed_("link watchdog");
        return;
      }

      // --- safety net 2: request timeout ---
      const uint32_t timeout = this->pin_inflight_ ? std::max(this->request_timeout_ms_, PIN_TIMEOUT_MS) : this->request_timeout_ms_;
      if (this->link_up_ && this->inflight_ > 0 && (now - this->last_activity_ms_) > timeout)
      {
        this->log_link_("request timeout, forcing disconnect", ESPHOME_LOG_LEVEL_WARN);
        this->link_failed_("no GATT response");
        return;
      }

      if (this->status_has_error())
      {
        this->status_clear_error();
        this->link_failed_("request could not be sent");
        return;
      }

      // --- transaction driver ---
      if (this->want_link_)
      {
        const bool opening = pst == ClientState::CONNECTING;
        if (!this->link_up_ && !opening)
        {
          if (this->pending_write_temperature_ && (now - this->temperature_requested_ms_) > COMMAND_TTL_MS)
            this->drop_writes_("not delivered within 24 h", true, false);
          if (this->pending_write_settings_ && (now - this->settings_requested_ms_) > COMMAND_TTL_MS)
            this->drop_writes_("not delivered within 24 h", false, true);

          if (!this->has_pending_())
          {
            this->want_link_ = false;
            this->teardown_link_("nothing pending", false);
            return;
          }

          if (!this->slow_phase_ && !this->in_fast_phase_(now))
          {
            this->slow_phase_ = true;
            this->failing_since_ms_ = now;
            this->publish_connection_(false);
            ESP_LOGW(TAG, "[%s] not reachable for %u s - keeping the request, retrying every %u s", this->get_name().c_str(), (unsigned)((now - this->txn_started_ms_) / 1000), (unsigned)(SLOW_RETRY_MS / 1000));
            if ((int32_t)(this->next_connect_ms_ - now) > (int32_t)SLOW_RETRY_MS)
              this->next_connect_ms_ = now + SLOW_RETRY_MS;
          }
        }

        if (!this->link_up_)
        {
          if (pst == ClientState::IDLE)
          {
            if ((int32_t)(now - this->next_connect_ms_) >= 0)
              this->try_connect_();
          }
          else if (pst == ClientState::DISCOVERED)
          {
            if (this->sibling_connecting_())
            {
              // Waiting behind another eTRV's connection attempt is normal (the tracker promotes
              // one client at a time); restart the fallback timer.
              this->connect_requested_ms_ = now;
            }
            else if ((now - this->connect_requested_ms_) > PROMOTION_FALLBACK_MS)
            {
              ESP_LOGW(TAG, "[%s] tracker did not promote the connection request within %u ms, connecting directly", this->get_name().c_str(), (unsigned)PROMOTION_FALLBACK_MS);
              this->connect_requested_ms_ = now;
              this->parent()->connect();
            }
          }
        }
        else if (this->node_state == ClientState::ESTABLISHED && this->inflight_ == 0)
        {
          this->issue_next_batch_();
        }
      }
      else if (this->link_up_ && this->node_state == ClientState::ESTABLISHED && this->inflight_ == 0)
      {
        // Unsolicited link (tracker auto_connect) and nothing to do: close it right away.
        this->finish_transaction_("nothing pending");
      }
    }

    // ------------------------------------------------------------------------------------------
    // requests from ESPHome
    // ------------------------------------------------------------------------------------------
    void Device::update()
    {
      if (this->xxtea->status() == XXTEA_STATUS_SUCCESS)
      {
        ESP_LOGD(TAG, "[%s] poll: requesting device state", this->get_name().c_str());
        this->pending_read_ = true;
      }
      else
      {
        ESP_LOGI(TAG, "[%s] poll: secret key unknown, will try to read it", this->get_name().c_str());
        this->pending_secret_key_ = true;
      }
      this->request_work_(false);
    }

    void Device::control(const ClimateCall &call)
    {
      if (this->xxtea->status() != XXTEA_STATUS_SUCCESS)
      {
        ESP_LOGE(TAG, "[%s] secret key unknown - cannot control the device", this->get_name().c_str());
        return;
      }
      const bool had_writes = this->has_writes_();
      bool changed = false;
      auto *t = this->p_temperature->get();
      auto *s = this->p_settings->get();
      const uint32_t now = millis();
      // The cached device state may be up to one poll interval old (the eTRV schedule or the dial may
      // have changed it since), so it is only trusted for de-duplication when it was read just now.
      const bool fresh = this->read_once_ && (now - this->last_read_ms_) < FRESH_READ_MS;

      if (call.get_target_temperature().has_value())
      {
        // The eTRV works in 0.5 C steps; round so that the written value can be confirmed by the
        // re-read (a 20.3 would be written as 20.5 and never "converge").
        float new_temp = roundf(*call.get_target_temperature() * 2.0f) / 2.0f;
        if (std::isnan(new_temp) || new_temp < 5.0f || new_temp > 30.0f)
        {
          ESP_LOGE(TAG, "[%s] rejecting out-of-range target temperature: %.1f C (allowed 5.0-30.0)", this->get_name().c_str(), *call.get_target_temperature());
        }
        else if (s != nullptr && (new_temp < s->temperature_min || new_temp > s->temperature_max))
        {
          ESP_LOGE(TAG, "[%s] rejecting target temperature %.1f C outside the device range %.1f-%.1f C", this->get_name().c_str(), new_temp, s->temperature_min, s->temperature_max);
        }
        else
        {
          // While paused the eTRV reports its frost temperature as the set point (it restores its
          // own manual set point when the pause ends), so that value is no basis for skipping a write.
          const bool paused = s != nullptr && s->is_paused();
          const float current = this->pending_write_temperature_ ? this->requested_target_temperature_ : ((t != nullptr && fresh && !paused) ? t->target_temperature : NAN);
          if (!std::isnan(current) && std::fabs(current - new_temp) < 0.05f)
          {
            ESP_LOGD(TAG, "[%s] target temperature unchanged (%.1f C), skipping write", this->get_name().c_str(), new_temp);
          }
          else
          {
            ESP_LOGD(TAG, "[%s] target temperature change: %.1f -> %.1f C", this->get_name().c_str(), !std::isnan(current) ? current : (t != nullptr ? t->target_temperature : NAN), new_temp);
            if (!this->pending_write_temperature_)
              this->target_before_request_ = this->target_temperature;
            this->temperature_requested_ms_ = now;
            this->requested_target_temperature_ = new_temp;
            this->pending_write_temperature_ = true;
            this->temperature_seq_++;
            changed = true;
          }
        }
      }

      if (call.get_mode().has_value())
      {
        // NOTE: mode (16-byte settings characteristic) and set point (8-byte temperature
        // characteristic) are written in separate BLE links. This is a conservative choice only:
        // the "mode switch resets the valve" reports (upstream issue #11) were caused by an upstream
        // bug that wrote 8 of the 16 settings bytes, see device_data.h.
        const ClimateMode new_mode = *call.get_mode();
        if (new_mode != ClimateMode::CLIMATE_MODE_HEAT && new_mode != ClimateMode::CLIMATE_MODE_AUTO && new_mode != ClimateMode::CLIMATE_MODE_OFF)
        {
          ESP_LOGE(TAG, "[%s] unsupported mode %d", this->get_name().c_str(), (int)new_mode);
        }
        else
        {
          const bool known = this->pending_write_settings_ || (s != nullptr && fresh);
          const ClimateMode current = this->pending_write_settings_ ? this->requested_mode_ : (s != nullptr ? s->device_mode : new_mode);
          if (known && current == new_mode)
          {
            ESP_LOGD(TAG, "[%s] mode unchanged (%d), skipping write", this->get_name().c_str(), (int)new_mode);
          }
          else
          {
            ESP_LOGD(TAG, "[%s] mode change: %d -> %d", this->get_name().c_str(), (int)current, (int)new_mode);
            if (!this->pending_write_settings_)
              this->mode_before_request_ = this->mode;
            this->settings_requested_ms_ = now;
            this->requested_mode_ = new_mode;
            this->pending_write_settings_ = true;
            this->settings_seq_++;
            changed = true;
          }
        }
      }

      if (!changed)
        return;
      // A user command gets the full number of attempts even after an earlier give-up.
      this->consecutive_hard_errors_ = 0;
      this->save_pending_();
      // Show the requested values right away; they stay shown until written (or dropped).
      this->publish_climate_state();
      this->request_work_(true, !had_writes);
    }

    void Device::publish_climate_state()
    {
      if (this->pending_write_temperature_)
        this->target_temperature = this->requested_target_temperature_;
      if (this->pending_write_settings_)
        this->mode = this->requested_mode_;
      // TODO action should consider the "open window detection" feature of Danfoss Eco
      if (this->mode == ClimateMode::CLIMATE_MODE_OFF)
        this->action = ClimateAction::CLIMATE_ACTION_OFF; // paused: frost protection only
      else if (!std::isnan(this->current_temperature) && !std::isnan(this->target_temperature))
        this->action = (this->current_temperature > this->target_temperature) ? ClimateAction::CLIMATE_ACTION_IDLE : ClimateAction::CLIMATE_ACTION_HEATING;
      this->publish_state();
    }

    void Device::publish_connection_(bool connected)
    {
      if (this->connection_ != nullptr && this->connected_published_ != (int8_t)connected)
        this->connection_->publish_state(connected);
      this->connected_published_ = connected;
    }

    // ------------------------------------------------------------------------------------------
    // transaction / link management
    // ------------------------------------------------------------------------------------------
    void Device::request_work_(bool user_command, bool first_write)
    {
      const uint32_t now = millis();
      if (!this->want_link_)
      {
        this->want_link_ = true;
        this->txn_started_ms_ = now;
        this->slow_phase_ = false;
        this->connect_attempts_ = 0;
        this->open_failures_ = 0;
        this->links_this_txn_ = 0;
        this->retry_count_ = 0;
        this->next_connect_ms_ = now;
        this->last_fast_rearm_ms_ = now;
      }
      else if (user_command && first_write && !this->slow_phase_)
      {
        // the first command of a transaction that was started by a poll gets a full fast window
        this->txn_started_ms_ = now;
      }
      else if (user_command && this->slow_phase_ && (now - this->last_fast_rearm_ms_) >= FAST_REARM_MIN_MS)
      {
        // A new request for an eTRV that has been unreachable for a while: try promptly again.
        ESP_LOGI(TAG, "[%s] new request - re-entering the fast retry phase", this->get_name().c_str());
        this->txn_started_ms_ = now;
        this->slow_phase_ = false;
        this->retry_count_ = 0;
        this->open_failures_ = 0;
        this->last_fast_rearm_ms_ = now;
        if (!this->link_up_)
          this->next_connect_ms_ = now;
      }
      // loop() requests the link (or, if a link is up, issues the new work on it)
    }

    void Device::try_connect_()
    {
      if (this->parent()->state() != ClientState::IDLE)
      {
        ESP_LOGV(TAG, "[%s] connection request deferred (parent=%s)", this->get_name().c_str(), client_state_str(this->parent()->state()));
        return; // loop() retries once the client is IDLE
      }
      if (this->parent()->get_gattc_if() == ESP_GATT_IF_NONE)
      {
        // GATT app not registered yet (right after boot / BLE restart); Bluedroid would drop the open
        this->next_connect_ms_ = millis() + 1000;
        return;
      }

      if (this->xxtea->status() == XXTEA_STATUS_NOT_INITIALIZED)
        ESP_LOGI(TAG, "[%s] Short press Danfoss Eco hardware button NOW in order to allow reading the secret key", this->get_name().c_str());

      // Service discovery costs 7-11 s of link time at the eTRV's slow connection interval. Discover
      // once (first link after boot, or after anything looked wrong at the remembered handles);
      // afterwards use ESPHome's V3_WITH_CACHE client mode, where the link is reported ESTABLISHED
      // right at OPEN and no discovery is issued. The secret key characteristic only exists during
      // the eTRV's pairing window, so onboarding always discovers.
      const bool discover = !this->handles_verified_ || this->xxtea->status() != XXTEA_STATUS_SUCCESS;
      this->parent()->set_connection_type(discover ? ConnectionType::V1 : ConnectionType::V3_WITH_CACHE);

      if (this->connect_attempts_ < 255)
        this->connect_attempts_++;
      this->connect_requested_ms_ = millis();
      if (!this->parent()->enabled)
        this->parent()->set_enabled(true);

      ESP_LOGD(TAG, "[%s] requesting BLE link (attempt %u, %s)", this->get_name().c_str(), (unsigned)this->connect_attempts_, discover ? "with service discovery" : "known handles, no discovery");
      // Hand the request to the tracker's promote loop (same mechanism bluetooth_proxy uses): it
      // stops the scan, raises the coexistence preference to Bluetooth and calls connect().
      this->parent()->set_state(ClientState::DISCOVERED);
    }

    bool Device::send_read_(DeviceProperty *p)
    {
      if (!p->read_request(this->parent()))
        return false;
      this->inflight_++;
      this->last_activity_ms_ = millis();
      return true;
    }

    bool Device::send_write_(WritableProperty *p, uint8_t *buff, uint16_t len)
    {
      if (!p->write_request(this->parent(), buff, len))
        return false;
      this->inflight_++;
      this->last_activity_ms_ = millis();
      return true;
    }

    void Device::issue_next_batch_()
    {
      this->batch_link_errors_ = 0;
      this->batch_hard_errors_ = 0;

      // 1. onboarding: read the secret key
      if (this->pending_secret_key_)
      {
        if (this->p_secret_key->has_handle())
        {
          this->batch_ = BATCH_SECRET_KEY;
          if (this->send_read_(this->p_secret_key.get()))
            return;
        }
        else
        {
          ESP_LOGW(TAG, "[%s] secret key characteristic not available (hardware button not pressed)", this->get_name().c_str());
        }
        this->pending_secret_key_ = false;
      }

      if (this->xxtea->status() != XXTEA_STATUS_SUCCESS)
      {
        ESP_LOGW(TAG, "[%s] secret key unknown - cannot read or write the device", this->get_name().c_str());
        this->pending_read_ = this->pending_time_sync_ = this->pending_e10_ack_ = false;
        if (this->has_writes_())
          this->drop_writes_("secret key unknown");
        this->finish_transaction_("secret key unknown");
        return;
      }

      // 2. read the device state - always before any write, so every write is packed from the state
      //    the eTRV holds right now (never from a copy that may be hours old)
      const bool need_read = this->pending_read_ || (!this->fresh_read_this_link_ && (this->has_writes_() || this->pending_time_sync_ || this->pending_e10_ack_));
      if (need_read)
      {
        this->batch_ = BATCH_READ;
        // All reads must be sent: a write must never be packed from a block of an earlier link.
        bool ok = this->send_read_(this->p_battery.get()) &&
                  this->send_read_(this->p_temperature.get()) &&
                  this->send_read_(this->p_settings.get()) &&
                  this->send_read_(this->p_errors.get());
        if (ok && this->time_sync_enabled_() && this->p_time->has_handle())
          ok = this->send_read_(this->p_time.get());
        if (!ok)
          this->link_failed_("read request could not be sent");
        return;
      }

      // 3. mode (16-byte settings): only byte 4 changes, all other bytes as read in this link
      if (this->pending_write_settings_ && !this->wrote_temperature_this_link_)
      {
        auto *s = this->p_settings->get();
        if (s == nullptr || !s->valid)
        {
          this->hard_error_("no valid settings block to modify");
          return;
        }
        const uint8_t new_raw = s->raw_mode_for(this->requested_mode_);
        if (new_raw == s->raw_mode)
        {
          ESP_LOGD(TAG, "[%s] device already in the requested mode (%s)", this->get_name().c_str(), SettingsData::mode_str(s->raw_mode));
          this->pending_write_settings_ = false;
          this->save_pending_();
          this->publish_climate_state();
        }
        else
        {
          uint8_t buff[16];
          if (!s->pack(buff, this->requested_mode_))
          {
            this->hard_error_("could not encrypt the settings block");
            return;
          }
          this->batch_ = BATCH_WRITE_SETTINGS;
          this->wrote_settings_this_link_ = true;
          this->settings_sent_seq_ = this->settings_seq_;
          ESP_LOGI(TAG, "[%s] writing mode %s -> %s", this->get_name().c_str(), SettingsData::mode_str(s->raw_mode), SettingsData::mode_str(new_raw));
          if (this->send_write_(this->p_settings.get(), buff, sizeof(buff)))
            return;
          this->link_failed_("settings write could not be sent");
          return;
        }
      }

      // 4. set point (8-byte temperature characteristic), never in the same link as a mode write
      if (this->pending_write_temperature_ && !this->wrote_settings_this_link_)
      {
        auto *t = this->p_temperature->get();
        if (t == nullptr || !t->valid)
        {
          this->hard_error_("no valid temperature block to modify");
          return;
        }
        auto *range = this->p_settings->get();
        if (range != nullptr && range->is_paused())
        {
          // The Danfoss app never writes a set point while the eTRV is paused (OFF); neither do we.
          ESP_LOGW(TAG, "[%s] the eTRV is paused (OFF) - not writing the set point %.1f C (switch to HEAT first)", this->get_name().c_str(), this->requested_target_temperature_);
          this->pending_write_temperature_ = false;
          this->save_pending_();
          this->target_temperature = t->target_temperature;
          this->publish_climate_state();
        }
        else if (range != nullptr && (this->requested_target_temperature_ < range->temperature_min || this->requested_target_temperature_ > range->temperature_max))
        {
          ESP_LOGE(TAG, "[%s] requested set point %.1f C is outside the device range %.1f-%.1f C, not writing it", this->get_name().c_str(), this->requested_target_temperature_, range->temperature_min, range->temperature_max);
          this->pending_write_temperature_ = false;
          this->save_pending_();
          this->target_temperature = t->target_temperature;
          this->publish_climate_state();
        }
        else if (std::fabs(t->target_temperature - this->requested_target_temperature_) < 0.05f)
        {
          ESP_LOGD(TAG, "[%s] device already has the requested set point (%.1f C)", this->get_name().c_str(), t->target_temperature);
          this->pending_write_temperature_ = false;
          this->save_pending_();
          this->publish_climate_state();
        }
        else
        {
          uint8_t buff[8];
          if (!t->pack(buff, this->requested_target_temperature_))
          {
            this->hard_error_("could not encrypt the temperature block");
            return;
          }
          this->batch_ = BATCH_WRITE_TEMPERATURE;
          this->wrote_temperature_this_link_ = true;
          this->temperature_sent_seq_ = this->temperature_seq_;
          ESP_LOGI(TAG, "[%s] writing set point %.1f -> %.1f C", this->get_name().c_str(), t->target_temperature, this->requested_target_temperature_);
          if (this->send_write_(this->p_temperature.get(), buff, sizeof(buff)))
            return;
          this->link_failed_("temperature write could not be sent");
          return;
        }
      }

      // 5. clock (app format: UTC epoch + offset, see TimeData)
#ifdef USE_TIME
      if (this->pending_time_sync_ && this->fresh_read_this_link_)
      {
        auto *td = this->p_time->get();
        int32_t epoch = 0, offset = 0;
        uint8_t buff[8];
        if (td == nullptr || !this->p_time->has_handle() || !this->expected_clock_(epoch, offset))
        {
          this->pending_time_sync_ = false;
        }
        else if (!td->pack(buff, epoch, offset))
        {
          this->pending_time_sync_ = false;
          ESP_LOGE(TAG, "[%s] could not encrypt the time", this->get_name().c_str());
        }
        else
        {
          this->batch_ = BATCH_WRITE_TIME;
          // best effort, one attempt: the rate limit in evaluate_clock_ decides the next one
          this->time_sync_attempted_ = true;
          this->last_time_sync_ms_ = millis();
          this->pending_time_sync_ = false;
          ESP_LOGI(TAG, "[%s] setting the eTRV clock (device utc=%d offset=%d s -> utc=%d offset=%d s)", this->get_name().c_str(), (int)td->epoch, (int)td->utc_offset, (int)epoch, (int)offset);
          if (this->send_write_(this->p_time.get(), buff, sizeof(buff)))
            return;
          this->link_failed_("time write could not be sent");
          return;
        }
      }
#endif
      this->pending_time_sync_ = this->pending_time_sync_ && this->time_sync_enabled_();

      // 6. acknowledge E10 ("invalid clock information") once the clock is right again - the eTRV
      //    never clears the flag itself (the Danfoss app does exactly this after a battery change)
      if (this->pending_e10_ack_)
      {
        this->pending_e10_ack_ = false;
        auto *e = this->p_errors->get();
        if (e != nullptr && e->E10_INVALID_TIME && this->clock_ok_this_link_ && this->fresh_read_this_link_ && this->p_errors->has_handle())
        {
          uint8_t buff[8];
          const uint16_t flags = e->flags & ~(uint16_t)(1u << ErrorsData::E10_INVALID_TIME_BIT);
          if (!e->tail_is_zero())
          {
            ESP_LOGW(TAG, "[%s] unexpected error block layout (%s) - not acknowledging E10", this->get_name().c_str(), format_hex_pretty(e->raw_, e->length).c_str());
          }
          else if (!e->pack(buff, flags))
          {
            ESP_LOGE(TAG, "[%s] could not encrypt the error flags", this->get_name().c_str());
          }
          else
          {
            this->batch_ = BATCH_WRITE_ERRORS;
            this->e10_ack_attempted_ = true;
            this->last_e10_ack_ms_ = millis();
            ESP_LOGI(TAG, "[%s] clock is set - acknowledging E10 (error flags %#06x -> %#06x)", this->get_name().c_str(), e->flags, flags);
            if (this->send_write_(this->p_errors.get(), buff, sizeof(buff)))
              return;
            this->link_failed_("error flags write could not be sent");
            return;
          }
        }
      }

      // 7. informational values, piggybacked on a link that has done all its real work (never a
      //    reason to open a link or to retry one)
      if (this->info_pending_ != 0 && this->fresh_read_this_link_ && !this->has_pending_() && this->issue_info_batch_())
        return;

      // Nothing (more) can be done on this link. If work is still pending (a set point deferred
      // behind a mode write) a fresh link follows shortly.
      this->finish_transaction_(this->has_pending_() ? "link done, more work pending" : "transaction complete");
    }

    void Device::finish_transaction_(const char *reason)
    {
      this->save_pending_(); // no-op unless the set of pending writes changed since the last save
      // optional clock / E10 work never justifies another link (re-derived by the next read)
      this->pending_time_sync_ = false;
      this->pending_e10_ack_ = false;
      const bool more = this->has_pending_();
      this->teardown_link_(reason, more);
      if (more)
      {
        this->want_link_ = true;
        this->next_connect_ms_ = millis() + RECONNECT_DELAY_MS;
      }
      else
      {
        this->want_link_ = false;
      }
    }

    void Device::teardown_link_(const char *reason, bool keep_enabled)
    {
      ESP_LOGD(TAG, "[%s] closing link (%s): age=%u ms inflight=%u keep_enabled=%d", this->get_name().c_str(), reason, (unsigned)this->link_age_ms_(), (unsigned)this->inflight_, (int)keep_enabled);
      this->inflight_ = 0;
      this->pin_inflight_ = false;
      this->batch_ = BATCH_NONE;
      this->link_up_ = false;
      this->fresh_read_this_link_ = false;
      this->clock_ok_this_link_ = false;
      // Optional clock / E10 work is re-derived by the next state read; it never keeps a link alive.
      this->pending_time_sync_ = false;
      this->pending_e10_ack_ = false;
      this->wrote_settings_this_link_ = false;
      this->wrote_temperature_this_link_ = false;
      this->node_state = ClientState::IDLE;

      // A request that is merely parked for the tracker's promote loop was never opened.
      if (this->parent()->state() == ClientState::DISCOVERED)
        this->parent()->set_state(ClientState::IDLE);

      if (!keep_enabled)
        this->parent()->set_enabled(false); // disconnects if a link is up/being opened
      const auto pst = this->parent()->state();
      if (pst == ClientState::CONNECTING || pst == ClientState::CONNECTED || pst == ClientState::ESTABLISHED)
        this->parent()->disconnect();
    }

    // A link failed for a reason that a new link may fix (radio, timeouts).
    void Device::link_failed_(const char *why)
    {
      this->teardown_link_(why, false);
      if (this->want_link_)
        this->schedule_retry_(why);
    }

    // The eTRV answered, but not in a usable way (rejected PIN/request, undecryptable data).
    void Device::hard_error_(const char *why)
    {
      if (this->consecutive_hard_errors_ < 255)
        this->consecutive_hard_errors_++;
      ESP_LOGE(TAG, "[%s] %s (protocol error %u/%u)", this->get_name().c_str(), why, (unsigned)this->consecutive_hard_errors_, (unsigned)MAX_HARD_ERRORS);
      if (this->consecutive_hard_errors_ >= MAX_HARD_ERRORS)
      {
        this->give_up_(why);
        return;
      }
      this->teardown_link_(why, false);
      if (this->want_link_)
        this->schedule_retry_(why);
    }

    void Device::schedule_retry_(const char *why)
    {
      const uint32_t now = millis();
      uint32_t delay;
      if (!this->slow_phase_ && this->in_fast_phase_(now))
      {
        const size_t last = sizeof(RETRY_BACKOFF_MS) / sizeof(RETRY_BACKOFF_MS[0]) - 1;
        const size_t idx = this->retry_count_ > last ? last : this->retry_count_;
        delay = RETRY_BACKOFF_MS[idx];
        if (this->retry_count_ < 255)
          this->retry_count_++;
      }
      else
      {
        delay = (this->slow_phase_ && (now - this->failing_since_ms_) > SLOW_LONG_AFTER_MS) ? SLOW_RETRY_LONG_MS : SLOW_RETRY_MS;
      }
      this->next_connect_ms_ = now + delay;
      ESP_LOGW(TAG, "[%s] %s - retry in %u s (%s phase, links=%u, failed opens=%u, requests=%u)", this->get_name().c_str(), why, (unsigned)(delay / 1000), this->slow_phase_ ? "slow" : "fast", (unsigned)this->links_this_txn_, (unsigned)this->open_failures_, (unsigned)this->connect_attempts_);
    }

    void Device::drop_writes_(const char *reason, bool temperature, bool settings)
    {
      temperature = temperature && this->pending_write_temperature_;
      settings = settings && this->pending_write_settings_;
      if (!temperature && !settings)
        return;
      ESP_LOGE(TAG, "[%s] dropping the requested %s%s%s (%s)", this->get_name().c_str(), temperature ? "set point" : "", temperature && settings ? " and " : "", settings ? "mode" : "", reason);
      // Show what the device really has again (or what was shown before the request, if the device
      // has not been read yet).
      auto *t = this->p_temperature->get();
      auto *s = this->p_settings->get();
      if (temperature)
      {
        if (t != nullptr)
          this->target_temperature = t->target_temperature;
        else if (!std::isnan(this->target_before_request_))
          this->target_temperature = this->target_before_request_;
        this->pending_write_temperature_ = false;
      }
      if (settings)
      {
        this->mode = s != nullptr ? s->device_mode : this->mode_before_request_;
        this->pending_write_settings_ = false;
      }
      this->save_pending_();
      this->publish_climate_state();
    }

    // Requested but undelivered writes survive a reboot / OTA (they are still checked against a
    // fresh read before being written). Only written to flash when the set of requests changes.
    void Device::save_pending_()
    {
      PendingWrites p{};
      p.flags = (this->pending_write_temperature_ ? 1 : 0) | (this->pending_write_settings_ ? 2 : 0);
      p.target = this->requested_target_temperature_;
      p.mode = (uint8_t)this->requested_mode_;
      if (p.flags == 0 && !this->pending_saved_)
        return;
      // (ESPHome coalesces preference saves in RAM and only writes changed data to flash)
      this->pending_pref_.save(&p);
      this->pending_saved_ = p.flags != 0;
    }

    void Device::load_pending_()
    {
      PendingWrites p{};
      if (!this->pending_pref_.load(&p) || p.flags == 0)
        return;
      // The time of the original request is unknown after a restart: give a resumed request one
      // hour (not another full COMMAND_TTL).
      const uint32_t now = millis() - (COMMAND_TTL_MS - RESUME_TTL_MS);
      if ((p.flags & 1) && !std::isnan(p.target) && p.target >= 5.0f && p.target <= 30.0f)
      {
        this->requested_target_temperature_ = p.target;
        this->pending_write_temperature_ = true;
        this->temperature_seq_++;
        this->temperature_requested_ms_ = now;
        this->target_before_request_ = this->target_temperature; // restored state
      }
      if ((p.flags & 2) && (p.mode == ClimateMode::CLIMATE_MODE_HEAT || p.mode == ClimateMode::CLIMATE_MODE_AUTO || p.mode == ClimateMode::CLIMATE_MODE_OFF))
      {
        this->requested_mode_ = (ClimateMode)p.mode;
        this->pending_write_settings_ = true;
        this->settings_seq_++;
        this->settings_requested_ms_ = now;
        this->mode_before_request_ = this->mode; // restored state
      }
      this->pending_saved_ = true;
      if (this->has_writes_())
        ESP_LOGI(TAG, "[%s] resuming requests from before the restart: set point=%s mode=%s", this->get_name().c_str(), this->pending_write_temperature_ ? "yes" : "no", this->pending_write_settings_ ? "yes" : "no");
    }

    void Device::give_up_(const char *reason)
    {
      ESP_LOGE(TAG, "[%s] giving up (%s): dropping pending read=%d set point=%d mode=%d key=%d time=%d - next attempt on the next poll or command", this->get_name().c_str(), reason, (int)this->pending_read_, (int)this->pending_write_temperature_, (int)this->pending_write_settings_, (int)this->pending_secret_key_, (int)this->pending_time_sync_);
      this->drop_writes_(reason);
      this->pending_read_ = false;
      this->pending_secret_key_ = false;
      this->pending_time_sync_ = false;
      this->pending_e10_ack_ = false;
      this->want_link_ = false;
      this->teardown_link_(reason, false);
      this->publish_connection_(false);
    }

    // ------------------------------------------------------------------------------------------
    // GATT client events
    // ------------------------------------------------------------------------------------------
    void Device::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
    {
      const uint32_t now = millis();
      switch (event)
      {
      case ESP_GATTC_CONNECT_EVT:
        if (memcmp(param->connect.remote_bda, this->parent()->get_remote_bda(), 6) != 0)
          return; // event does not belong to this client, exit gattc_event_handler

        this->link_up_ = true;
        this->link_started_ms_ = now;
        this->last_activity_ms_ = now;
        this->inflight_ = 0;
        this->batch_ = BATCH_NONE;
        this->fresh_read_this_link_ = false;
        this->clock_ok_this_link_ = false;
        this->wrote_settings_this_link_ = false;
        this->wrote_temperature_this_link_ = false;
        if (this->want_link_ && this->links_this_txn_ < 255)
          this->links_this_txn_++;
        ESP_LOGD(TAG, "[%s] connect, conn_id=%d", this->get_name().c_str(), param->connect.conn_id);
        break;

      case ESP_GATTC_OPEN_EVT:
        this->last_open_status_ = param->open.status;
        this->last_open_conn_id_ = param->open.conn_id;
        if (param->open.status == ESP_GATT_OK)
        {
          ESP_LOGD(TAG, "[%s] open OK, conn_id=%d, mtu=%d (%u ms after connect)", this->get_name().c_str(), param->open.conn_id, param->open.mtu, (unsigned)this->link_age_ms_());
          // V3_WITH_CACHE: BLEClientBase has just set itself ESTABLISHED and will not discover
          // services, so the link is ready for us right now.
          if (this->parent()->state() == ClientState::ESTABLISHED)
            this->on_link_ready_();
        }
        else if (param->open.status == ESP_GATT_ALREADY_OPEN)
        {
          // Bluedroid still holds a link to the eTRV (a close that never completed). ESPHome treats
          // this as an open without CONNECT_EVT and has no conn_id to close it: close it right away.
          ESP_LOGW(TAG, "[%s] open returned ALREADY_OPEN (conn_id=%d) - closing the stale link", this->get_name().c_str(), param->open.conn_id);
          this->link_up_ = false;
          this->inflight_ = 0;
          if (this->parent()->get_conn_id() != NO_CONN_ID)
          {
            this->parent()->disconnect();
          }
          else
          {
            esp_ble_gattc_close(gattc_if, param->open.conn_id);
            this->parent()->set_state(ClientState::IDLE);
          }
          if (this->want_link_)
            this->schedule_retry_("stale link closed");
          else
            this->parent()->set_enabled(false);
        }
        else
        {
          // Nothing usable was established (e.g. status 0x85 after the 20 s open timeout: the eTRV
          // was not heard). The first time the ble_client stays enabled so the tracker's
          // auto_connect can connect the moment the eTRV is heard advertising; after that it is
          // parked between the backed-off attempts, because every 20 s open attempt of an eTRV at
          // the edge of the range delays the other thermostats (one connection at a time).
          this->link_up_ = false;
          this->inflight_ = 0;
          if (this->open_failures_ < 255)
            this->open_failures_++;
          ESP_LOGW(TAG, "[%s] failed to open, status=%#04x (failed opens %u)", this->get_name().c_str(), param->open.status, (unsigned)this->open_failures_);
          if (!this->want_link_)
          {
            this->parent()->set_enabled(false); // unsolicited auto_connect attempt, nothing to do
          }
          else
          {
            if (this->open_failures_ >= 2)
              this->parent()->set_enabled(false);
            if (this->retry_count_ == 0)
              this->retry_count_ = 1; // the open itself already took 20 s: start the ladder at 15 s
            this->schedule_retry_(this->open_failures_ >= 2 ? "open failed again, client parked until the next attempt" : "open failed, client stays enabled for auto_connect");
          }
        }
        break;

      case ESP_GATTC_CLOSE_EVT:
      case ESP_GATTC_DISCONNECT_EVT:
      {
        const int reason = event == ESP_GATTC_CLOSE_EVT ? (int)param->close.reason : (int)param->disconnect.reason;
        const uint8_t lost = this->inflight_;
        // teardown_link_() clears link_up_ before the stack reports the close, so a link that is
        // still marked up here was lost unexpectedly (supervision timeout, peer disconnect, ...)
        const bool unexpected = this->link_up_;
        // Optional clock / E10 work is re-derived by the next state read; it never keeps a link alive.
        this->pending_time_sync_ = false;
        this->pending_e10_ack_ = false;
        // A link lost while only optional work (clock, E10, informational reads) was in flight has
        // done all its real work: nothing to retry.
        const bool info_only = !this->has_pending_();
        ESP_LOGD(TAG, "[%s] %s, conn_id=%d, reason=%#04x, link_age=%u ms", this->get_name().c_str(), event == ESP_GATTC_CLOSE_EVT ? "close" : "disconnect", event == ESP_GATTC_CLOSE_EVT ? param->close.conn_id : param->disconnect.conn_id, reason, (unsigned)this->link_age_ms_());
        this->link_up_ = false;
        this->inflight_ = 0;
        this->batch_ = BATCH_NONE;
        this->fresh_read_this_link_ = false;
        this->clock_ok_this_link_ = false;
        this->wrote_settings_this_link_ = false;
        this->wrote_temperature_this_link_ = false;
        this->node_state = ClientState::IDLE;
        if (lost > 0 && info_only)
          ESP_LOGD(TAG, "[%s] link lost during optional work (reason=%#04x) - done again on a later link", this->get_name().c_str(), reason);
        else if (lost > 0)
          ESP_LOGW(TAG, "[%s] link lost with %u unanswered request(s) (reason=%#04x)%s", this->get_name().c_str(), (unsigned)lost, reason, this->want_link_ ? " - pending work will be retried" : "");
        if (info_only && this->want_link_)
        {
          // the transaction is complete: nothing to retry
          this->want_link_ = false;
          if (this->parent()->enabled)
            this->parent()->set_enabled(false);
        }
        else if (this->want_link_ && unexpected)
        {
          // Unexpected loss in the middle of our work. The first time, stay enabled so the
          // tracker's auto_connect reconnects the moment the eTRV advertises again; from the
          // second loss on, park the client and back off (every link costs the eTRV battery).
          if (this->links_this_txn_ >= 2)
            this->parent()->set_enabled(false);
          this->schedule_retry_("link lost mid-transaction");
        }
        else if (this->want_link_)
        {
          if ((int32_t)(this->next_connect_ms_ - now) < (int32_t)RECONNECT_DELAY_MS)
            this->next_connect_ms_ = now + RECONNECT_DELAY_MS;
        }
        else if (this->parent()->enabled)
        {
          // nothing to do: make sure the tracker will not reconnect on its own
          this->parent()->set_enabled(false);
        }
        break;
      }

      case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK)
        {
          ESP_LOGW(TAG, "[%s] service discovery failed, status=%#04x", this->get_name().c_str(), param->search_cmpl.status);
          this->link_failed_("service discovery failed");
          break;
        }
        ESP_LOGD(TAG, "[%s] service discovery complete (%u ms after connect)", this->get_name().c_str(), (unsigned)this->link_age_ms_());
        for (auto p : this->properties)
          p->init_handle(this->parent());

        this->handles_verified_ = this->p_pin->has_handle() && this->p_battery->has_handle() && this->p_temperature->has_handle() && this->p_settings->has_handle() && this->p_errors->has_handle();
        if (!this->handles_verified_)
        {
          ESP_LOGE(TAG, "[%s] required characteristics not found (pin=%#04x battery=%#04x temperature=%#04x settings=%#04x errors=%#04x) - is this a Danfoss Eco?", this->get_name().c_str(), this->p_pin->handle, this->p_battery->handle, this->p_temperature->handle, this->p_settings->handle, this->p_errors->handle);
          this->hard_error_("required characteristics not found");
          break;
        }
        ESP_LOGD(TAG, "[%s] handles: pin=%#04x battery=%#04x temperature=%#04x settings=%#04x errors=%#04x time=%#04x key=%#04x", this->get_name().c_str(), this->p_pin->handle, this->p_battery->handle, this->p_temperature->handle, this->p_settings->handle, this->p_errors->handle, this->p_time->handle, this->p_secret_key->handle);
        this->on_link_ready_();
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

    // The link is usable (services discovered, or known handles without discovery): send the PIN.
    bool Device::on_link_ready_()
    {
      if (!this->want_link_)
      {
        // Unsolicited link (tracker auto_connect) with nothing to do: close it before the PIN.
        this->finish_transaction_("unsolicited link, nothing pending");
        return false;
      }
      if (!this->handles_verified_)
      {
        // Cannot happen unless ESPHome changed the V3_WITH_CACHE semantics; never write to guessed handles.
        ESP_LOGE(TAG, "[%s] link ready without verified handles - discovering on the next link", this->get_name().c_str());
        this->link_failed_("handles unknown");
        return false;
      }
      this->write_pin();
      return true;
    }

    // Something at the remembered handles did not look like a Danfoss Eco (ATT error or unexpected
    // value length): forget them, the next link discovers again.
    void Device::invalidate_handles_(const char *why)
    {
      if (this->handles_verified_)
        ESP_LOGW(TAG, "[%s] %s - forgetting GATT handles, next link will rediscover services", this->get_name().c_str(), why);
      this->handles_verified_ = false;
    }

    void Device::write_pin()
    {
      ESP_LOGV(TAG, "[%s] writing pin (handle=%#04x)", this->get_name().c_str(), this->p_pin->handle);

      uint8_t pin_bytes[sizeof(uint32_t)];
      write_int(pin_bytes, 0, this->pin_code_);

      if (!this->p_pin->write_request(this->parent(), pin_bytes, sizeof(pin_bytes), false))
      {
        this->status_set_error(); // loop() tears the link down
        return;
      }
      this->pin_inflight_ = true;
      this->inflight_++;
      this->last_activity_ms_ = millis();
    }

    void Device::on_write_pin(esp_ble_gattc_cb_param_t::gattc_write_evt_param param)
    {
      this->pin_inflight_ = false;
      if (this->inflight_ > 0)
        this->inflight_--;
      this->last_activity_ms_ = millis();

      if (param.status != ESP_GATT_OK)
      {
        ESP_LOGE(TAG, "[%s] pin FAILED, status=%#04x (link_age=%u ms)", this->get_name().c_str(), param.status, (unsigned)this->link_age_ms_());
        if (is_transient_status(param.status))
        {
          // the link died underneath the request: a radio problem, retried like any link loss
          this->link_failed_("PIN write failed");
        }
        else
        {
          // the eTRV rejected it (wrong pin_code or wrong handle layout): rediscover, count it
          this->invalidate_handles_("PIN write rejected");
          this->hard_error_("PIN rejected - check pin_code");
        }
        return;
      }

      ESP_LOGV(TAG, "[%s] pin OK (%u ms after connect)", this->get_name().c_str(), (unsigned)this->link_age_ms_());
      this->node_state = ClientState::ESTABLISHED; // loop() issues the pending operations
    }

    void Device::on_read(esp_ble_gattc_cb_param_t::gattc_read_char_evt_param param)
    {
      if (this->inflight_ > 0)
        this->inflight_--;
      else
        ESP_LOGW(TAG, "[%s] unexpected read response (handle=%#04x)", this->get_name().c_str(), param.handle);
      this->last_activity_ms_ = millis();
      ESP_LOGV(TAG, "[%s] read rsp: handle=%#04x status=%#04x len=%u inflight=%u link_age=%u ms", this->get_name().c_str(), param.handle, param.status, (unsigned)param.value_len, (unsigned)this->inflight_, (unsigned)this->link_age_ms_());

      auto found = find_if(properties.begin(), properties.end(),
                           [&param](const shared_ptr<DeviceProperty> &p)
                           { return p->handle == param.handle; });
      DeviceProperty *prop = found != properties.end() ? found->get() : nullptr;
      // Informational properties never fail the link, never count as protocol errors and never
      // invalidate the handles; an unusable answer just marks the value as unavailable until reboot.
      const bool informational = prop != nullptr && !prop->essential();
      const int info_idx = informational ? this->info_index_(prop) : -1;

      if (param.status != ESP_GATT_OK)
      {
        ESP_LOGW(TAG, "[%s] failed to read characteristic: handle=%#04x, status=%#04x", this->get_name().c_str(), param.handle, param.status);
        if (is_transient_status(param.status))
        {
          this->batch_link_errors_++;
        }
        else if (informational)
        {
          if (info_idx >= 0)
            this->info_pending_ &= ~(uint16_t)(1u << info_idx);
        }
        else
        {
          this->invalidate_handles_("read rejected by the device");
          this->batch_hard_errors_++;
        }
      }
      else if (prop == nullptr)
      {
        ESP_LOGW(TAG, "[%s] unknown property with handle=%#04x", this->get_name().c_str(), param.handle);
      }
      else if (!prop->accepts_length(param.value_len))
      {
        // Never feed an unexpected payload to the parsers; a wrong length also means the handle
        // does not carry what we think it does. A 1-byte payload on an encrypted characteristic is
        // the eTRV's error code (wrong PIN / not paired).
        if (param.value_len == 1 && prop->expected_length() > 1)
          ESP_LOGE(TAG, "[%s] characteristic handle=%#04x returned the 1-byte error code %#04x - wrong pin_code or device not paired?", this->get_name().c_str(), param.handle, param.value[0]);
        else
          ESP_LOGW(TAG, "[%s] characteristic handle=%#04x returned %u bytes, expected %u - ignoring", this->get_name().c_str(), param.handle, (unsigned)param.value_len, (unsigned)prop->expected_length());
        if (informational)
        {
          if (info_idx >= 0)
            this->info_pending_ &= ~(uint16_t)(1u << info_idx);
        }
        else
        {
          this->invalidate_handles_("unexpected value length");
          this->batch_hard_errors_++;
        }
      }
      else if (!prop->update_state(param.value, param.value_len))
      {
        if (informational)
        {
          if (info_idx >= 0)
            this->info_pending_ &= ~(uint16_t)(1u << info_idx);
        }
        else
        {
          this->batch_hard_errors_++;
        }
      }
      else if (info_idx >= 0)
      {
        this->info_pending_ &= ~(uint16_t)(1u << info_idx);
      }

      if (this->inflight_ == 0 && this->link_up_)
        this->on_batch_complete_();
    }

    void Device::on_batch_complete_()
    {
      const uint8_t batch = this->batch_;
      const uint8_t hard = this->batch_hard_errors_;
      const uint8_t transient = this->batch_link_errors_;
      this->batch_ = BATCH_NONE;
      this->batch_hard_errors_ = 0;
      this->batch_link_errors_ = 0;

      if (batch == BATCH_INFO)
      {
        // schedule: publish once all three parts of the current week have been read
        if ((this->info_batch_mask_ & INFO_BITS_SCHEDULE) != 0 && (this->info_pending_ & INFO_BITS_SCHEDULE) == 0 &&
            this->p_schedule[0]->get() != nullptr && this->p_schedule[1]->get() != nullptr && this->p_schedule[2]->get() != nullptr)
          this->publish_schedule_();
        this->info_batch_mask_ = 0;
        // Informational reads are optional: whatever failed is tried again on a later link that
        // has real work to do. Close this one now.
        this->finish_transaction_(transient > 0 ? "informational reads incomplete, retried on a later link" : "transaction complete");
        return;
      }
      if (hard > 0)
      {
        this->hard_error_(batch == BATCH_SECRET_KEY ? "secret key read failed" : "device returned unusable data");
        return;
      }
      if (transient > 0)
      {
        this->link_failed_("read failed");
        return;
      }

      if (batch == BATCH_READ)
      {
        this->pending_read_ = false;
        this->fresh_read_this_link_ = true;
        this->read_once_ = true;
        this->last_read_ms_ = millis();
        // a complete, plausible read: the device is reachable and the configuration is right
        this->consecutive_hard_errors_ = 0;
        this->publish_connection_(true);
        // The retry ladder is only reset by real progress: a write that keeps failing after good
        // reads must still back off (and reach the slow phase).
        if (!this->has_writes_() && !this->pending_time_sync_)
        {
          if (this->slow_phase_)
            ESP_LOGI(TAG, "[%s] reachable again", this->get_name().c_str());
          this->slow_phase_ = false;
          this->retry_count_ = 0;
          this->open_failures_ = 0;
          this->txn_started_ms_ = millis();
        }
        this->evaluate_clock_();
        this->publish_problems_();
        // the weekly schedule is re-read once a day (it can only change through the Danfoss app)
        if ((this->info_wanted_mask_() & INFO_BITS_SCHEDULE) != 0 && this->schedule_read_once_ && (millis() - this->schedule_read_ms_) > SCHEDULE_REFRESH_MS)
          this->info_pending_ |= INFO_BITS_SCHEDULE;
      }
      else if (batch == BATCH_SECRET_KEY)
      {
        this->pending_secret_key_ = false;
        if (this->xxtea->status() == XXTEA_STATUS_SUCCESS)
          this->pending_read_ = true; // key acquired: read the state in the same link
      }
    }

    // What the eTRV clock should hold right now, in the app's format (see TimeData): the UTC epoch
    // and the STANDARD-time offset (TimeZone.getRawOffset()), exactly as the Danfoss app writes it.
    // The eTRV adds the summer-time hour itself when its "automatic summer time" flag (settings
    // bit 1) is set; with the flag off it runs on standard time all year - as with the app.
    bool Device::expected_clock_(int32_t &epoch, int32_t &offset)
    {
#ifdef USE_TIME
      if (this->time_ == nullptr)
        return false;
      ESPTime utc = this->time_->utcnow();
      if (!utc.is_valid())
        return false; // no valid time source (yet)
      epoch = (int32_t)utc.timestamp;
      offset = ESPTime::timezone_offset();
#ifdef USE_TIME_TIMEZONE
      const auto &tz = time::get_global_tz();
      offset = -tz.std_offset_seconds;
      auto *s = this->p_settings->get();
      if (s != nullptr && !this->dst_warning_logged_ && s->get_daylight_saving() != tz.has_dst())
      {
        this->dst_warning_logged_ = true;
        if (tz.has_dst())
          ESP_LOGI(TAG, "[%s] automatic summer time is off in this eTRV: it runs on standard time (1 h behind in summer) - it can be switched on in the Danfoss app", this->get_name().c_str());
        else
          ESP_LOGW(TAG, "[%s] the eTRV applies summer time itself, but the configured timezone has none - its clock will be 1 h off in summer (switch it off in the Danfoss app)", this->get_name().c_str());
      }
#endif
      return true;
#else
      return false;
#endif
    }

    // Called after every complete read of the device state: decides whether the clock must be set
    // and whether a latched E10 can be acknowledged.
    void Device::evaluate_clock_()
    {
#ifdef USE_TIME
      this->clock_ok_this_link_ = false;
      auto *td = this->p_time->get();
      int32_t epoch = 0, offset = 0;
      if (td == nullptr || !this->expected_clock_(epoch, offset))
        return;
      const uint32_t now = millis();
      // 64-bit: the eTRV may report any epoch (garbage after a reset)
      const int64_t drift64 = std::llabs((int64_t)td->epoch - (int64_t)epoch);
      const int32_t drift = drift64 > INT32_MAX ? INT32_MAX : (int32_t)drift64;
      if (drift <= TIME_DRIFT_MAX_S && td->utc_offset == offset)
      {
        this->clock_ok_this_link_ = true;
        this->clock_lost_ = false;
      }
      else
      {
        if (this->clock_set_once_ && (drift > CLOCK_LOST_S || td->epoch == 0))
        {
          // Set successfully earlier, now far off again: the eTRV does not keep its clock (it raises
          // no E10 for this). Reported through clock_error / problems until the clock holds.
          if (!this->clock_lost_)
            ESP_LOGW(TAG, "[%s] the eTRV lost its clock again (utc=%d) - its schedule and vacations cannot work", this->get_name().c_str(), (int)td->epoch);
          this->clock_lost_ = true;
        }
        if (!this->pending_time_sync_ && (!this->time_sync_attempted_ || (now - this->last_time_sync_ms_) >= TIME_SYNC_MIN_INTERVAL_MS))
        {
          ESP_LOGD(TAG, "[%s] eTRV clock off by %d s (offset %d, expected %d) - will set it", this->get_name().c_str(), (int)drift, (int)td->utc_offset, (int)offset);
          this->pending_time_sync_ = true;
        }
      }
      auto *e = this->p_errors->get();
      if (e != nullptr && e->E10_INVALID_TIME && !this->pending_e10_ack_ && (!this->e10_ack_attempted_ || (now - this->last_e10_ack_ms_) >= E10_ACK_MIN_INTERVAL_MS))
      {
        // done in this link once the clock is right (after the sync, if one is needed)
        if (this->clock_ok_this_link_ || this->pending_time_sync_)
          this->pending_e10_ack_ = true;
      }
#endif
    }

    // Error entities, published after every complete state read (error flags + the clock state).
    void Device::publish_problems_()
    {
      auto *e = this->p_errors->get();
      if (e == nullptr)
        return;
      const bool lost = this->clock_lost_;
      if (this->problems_ != nullptr)
        this->problems_->publish_state(e->flags != 0 || lost);
      if (this->low_battery_ != nullptr)
        this->low_battery_->publish_state(e->E14_LOW_BATTERY || e->E15_VERY_LOW_BATTERY);
      if (this->valve_error_ != nullptr)
        this->valve_error_->publish_state(e->E9_VALVE_DOES_NOT_CLOSE);
      if (this->motor_error_ != nullptr)
        this->motor_error_->publish_state(e->has_code(6));
      if (this->clock_error_ != nullptr)
        this->clock_error_->publish_state(e->E10_INVALID_TIME || lost);
      if (this->hardware_error_ != nullptr)
        this->hardware_error_->publish_state(e->hardware_fault());

      // Human-readable list for the optional `problems_detail` text sensor.
      string problems;
      for (uint8_t code = 1; code <= 16; code++)
      {
        if (!e->has_code(code))
          continue;
        if (!problems.empty())
          problems += " | ";
        problems += ErrorsData::title(code);
      }
      if (lost)
        problems += problems.empty() ? "Clock Not Kept" : " | Clock Not Kept";
      // warn once per change, not on every read of a latched flag
      if (!this->errors_reported_ || e->flags != this->reported_error_flags_ || lost != this->reported_clock_lost_)
      {
        if (!problems.empty())
          ESP_LOGW(TAG, "[%s] eTRV reports: %s", this->get_name().c_str(), problems.c_str());
        else if (this->errors_reported_)
          ESP_LOGI(TAG, "[%s] eTRV reports no errors", this->get_name().c_str());
        this->errors_reported_ = true;
        this->reported_error_flags_ = e->flags;
        this->reported_clock_lost_ = lost;
      }
      if (this->problems_detail_ != nullptr)
        this->problems_detail_->publish_state(problems);
    }

    // Which informational values are read: those with a configured sensor, plus the firmware
    // revision (always, for the logs / dump_config).
    uint16_t Device::info_wanted_mask_() const
    {
      uint16_t mask = 1u << INFO_FIRMWARE;
      for (uint8_t f = 0; f < INFO_COUNT; f++)
      {
        if (this->info_sensors_[f] != nullptr)
          mask |= 1u << f;
      }
      if (this->thermostat_name_ != nullptr)
        mask |= INFO_BIT_NAME;
      if (this->pin_protection_ != nullptr)
        mask |= INFO_BIT_PIN;
      if (this->schedule_ != nullptr || this->schedule_home_temperature_ != nullptr || this->schedule_away_temperature_ != nullptr)
        mask |= INFO_BITS_SCHEDULE;
      return mask;
    }

    int Device::info_index_(const DeviceProperty *p) const
    {
      for (uint8_t f = 0; f < INFO_COUNT; f++)
      {
        if (this->p_info[f].get() == p)
          return f;
      }
      if (this->p_name.get() == p)
        return 6;
      if (this->p_pin_settings.get() == p)
        return 7;
      for (uint8_t part = 0; part < 3; part++)
      {
        if (this->p_schedule[part].get() == p)
          return 8 + part;
      }
      return -1;
    }

    // Sends the pending informational reads (all at once, like the state read). False if there was
    // nothing that could be read (characteristics this unit does not have are dropped for good).
    bool Device::issue_info_batch_()
    {
      this->batch_ = BATCH_INFO;
      this->info_batch_mask_ = 0;
      for (int bit = 0; bit < 11; bit++)
      {
        const uint16_t m = 1u << bit;
        if ((this->info_pending_ & m) == 0)
          continue;
        DeviceProperty *p = nullptr;
        if (bit < INFO_COUNT)
          p = this->p_info[bit].get();
        else if (bit == 6)
          p = this->p_name.get();
        else if (bit == 7)
          p = this->p_pin_settings.get();
        else
          p = this->p_schedule[bit - 8].get();
        if (p == nullptr || !p->has_handle())
        {
          this->info_pending_ &= ~m; // not offered by this unit
          continue;
        }
        if (!this->send_read_(p))
          break; // the stack refused (link going down): try the rest on a later link
        this->info_batch_mask_ |= m;
      }
      if (this->inflight_ > 0)
      {
        ESP_LOGD(TAG, "[%s] reading informational values (%#05x)", this->get_name().c_str(), (unsigned)this->info_batch_mask_);
        return true;
      }
      this->batch_ = BATCH_NONE;
      return false;
    }

    // "Mo-Fr 06:00-08:00,16:00-22:00; Sa-Su 07:00-23:00" (the "at home" periods of every day;
    // "away" for a day without any). Consecutive days with the same program are merged.
    void Device::publish_schedule_()
    {
      static const char *const DAYS[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
      string day_text[7];
      for (uint8_t part = 0; part < 3; part++)
      {
        auto *d = this->p_schedule[part]->get();
        for (uint8_t i = 0; i < d->day_count(); i++)
        {
          const uint8_t *periods = d->day(i);
          string text;
          for (uint8_t k = 0; k < 3; k++)
          {
            const uint8_t from = periods[2 * k], to = periods[2 * k + 1];
            if (to <= from)
              continue; // unused period
            char buf[16];
            snprintf(buf, sizeof(buf), "%02u:%02u-%02u:%02u", (unsigned)(from / 2), (unsigned)((from % 2) * 30), (unsigned)(to / 2), (unsigned)((to % 2) * 30));
            if (!text.empty())
              text += ",";
            text += buf;
          }
          day_text[d->first_day() + i] = text.empty() ? "away" : text;
        }
      }
      string out;
      for (uint8_t day = 0; day < 7;)
      {
        uint8_t last = day;
        while (last + 1 < 7 && day_text[last + 1] == day_text[day])
          last++;
        if (!out.empty())
          out += "; ";
        out += DAYS[day];
        if (last != day)
        {
          out += "-";
          out += DAYS[last];
        }
        out += " ";
        out += day_text[day];
        day = last + 1;
      }
      auto *first = this->p_schedule[0]->get();
      ESP_LOGD(TAG, "[%s] schedule: home %.1f C, away %.1f C, %s", this->get_name().c_str(), first->home_temperature(), first->away_temperature(), out.c_str());
      if (out.size() > 255)
        out = out.substr(0, 252) + "..."; // Home Assistant state limit
      if (this->schedule_ != nullptr)
        this->schedule_->publish_state(out);
      if (this->schedule_home_temperature_ != nullptr)
        this->schedule_home_temperature_->publish_state(first->raw_[0] > 0 ? first->home_temperature() : NAN);
      if (this->schedule_away_temperature_ != nullptr)
        this->schedule_away_temperature_->publish_state(first->raw_[1] > 0 ? first->away_temperature() : NAN);
      this->schedule_read_ms_ = millis();
      this->schedule_read_once_ = true;
    }

    void Device::on_write(esp_ble_gattc_cb_param_t::gattc_write_evt_param param)
    {
      if (this->inflight_ > 0)
        this->inflight_--;
      else
        ESP_LOGW(TAG, "[%s] unexpected write response (handle=%#04x)", this->get_name().c_str(), param.handle);
      this->last_activity_ms_ = millis();
      ESP_LOGD(TAG, "[%s] write rsp: handle=%#04x status=%#04x link_age=%u ms", this->get_name().c_str(), param.handle, param.status, (unsigned)this->link_age_ms_());

      const bool is_temperature = param.handle == this->p_temperature->handle;
      const bool is_settings = param.handle == this->p_settings->handle;
      const bool is_time = this->p_time->has_handle() && param.handle == this->p_time->handle;
      const bool is_errors = param.handle == this->p_errors->handle;
      this->batch_ = BATCH_NONE;

      if (param.status == ESP_GATT_OK)
      {
        // The cached block predates this write: not usable for de-duplication until the re-read.
        if (is_temperature || is_settings)
          this->read_once_ = false;
        // A newer value requested while this write was in flight keeps the flag set, so it is
        // written again.
        if (is_temperature && this->temperature_sent_seq_ == this->temperature_seq_)
          this->pending_write_temperature_ = false;
        if (is_settings && this->settings_sent_seq_ == this->settings_seq_)
          this->pending_write_settings_ = false;
        if (is_time)
        {
          this->pending_time_sync_ = false;
          this->clock_ok_this_link_ = true;
          this->clock_set_once_ = true;
        }
        this->save_pending_();
        if (!this->has_writes_())
        {
          // all requested writes delivered: this is progress, reset the retry ladder
          this->slow_phase_ = false;
          this->retry_count_ = 0;
          this->open_failures_ = 0;
          this->txn_started_ms_ = millis();
        }
        // re-read so the published state reflects the device (not needed after a clock write)
        if (is_temperature || is_settings || is_errors)
          this->pending_read_ = true;
      }
      else if (is_time)
      {
        // The clock is optional: a failed or rejected clock write is never a reason to fail the
        // link or to retry (rate-limited by TIME_SYNC_MIN_INTERVAL_MS).
        ESP_LOGW(TAG, "[%s] clock write failed, status=%#04x", this->get_name().c_str(), param.status);
        this->pending_time_sync_ = false;
      }
      else if (is_errors)
      {
        // Optional as well (rate-limited by E10_ACK_MIN_INTERVAL_MS).
        ESP_LOGW(TAG, "[%s] error flags write failed, status=%#04x", this->get_name().c_str(), param.status);
        this->pending_e10_ack_ = false;
      }
      else if (is_transient_status(param.status))
      {
        // the link died underneath the write: keep it pending, retry on a new link
        this->link_failed_("write failed");
      }
      else
      {
        ESP_LOGE(TAG, "[%s] write rejected by the device: handle=%#04x, status=%#04x", this->get_name().c_str(), param.handle, param.status);
        auto *t = this->p_temperature->get();
        auto *s = this->p_settings->get();
        // Only drop the request if the rejected write carried the latest value.
        if (is_temperature && this->temperature_sent_seq_ == this->temperature_seq_)
        {
          this->pending_write_temperature_ = false;
          if (t != nullptr)
            this->target_temperature = t->target_temperature;
        }
        if (is_settings && this->settings_sent_seq_ == this->settings_seq_)
        {
          this->pending_write_settings_ = false;
          if (s != nullptr)
            this->mode = s->device_mode;
        }
        this->save_pending_();
        this->publish_climate_state();
        this->pending_read_ = true;
        this->invalidate_handles_("write rejected by the device");
        this->hard_error_("write rejected by the device");
      }
    }

    // ------------------------------------------------------------------------------------------
    // configuration
    // ------------------------------------------------------------------------------------------
    void Device::set_pin_code(const string &str)
    {
      if (str.length() > 0)
        this->pin_code_ = atoi((const char *)str.c_str());
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
        if (str.length() != SECRET_KEY_LENGTH * 2 || !parse_hex_str(str.c_str(), SECRET_KEY_LENGTH * 2, buff))
        {
          ESP_LOGE(TAG, "[%s] secret_key must be 32 hex characters", this->get_name().c_str());
          this->mark_failed();
          return;
        }
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
      // never log the key itself (it is marked sensitive in the configuration); only during the
      // pairing window is it printed once, so that it can be put into the YAML
      ESP_LOGV(TAG, "[%s] secret_key set (%u bytes)", this->get_name().c_str(), (unsigned)SECRET_KEY_LENGTH);

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
