#pragma once

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/climate/climate.h"

#include "esphome/core/preferences.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "helpers.h"
#include "properties.h"
#include "my_component.h"
#include "xxtea.h"

#ifdef USE_ESP32

#include <set>
#include <esp_gattc_api.h>

namespace esphome
{
  namespace danfoss_eco
  {
    using namespace std;
    using namespace climate;

    // ------------------------------------------------------------------------------------------
    // BLE connection lifecycle ("transaction" model)
    //
    // The eTRV is a battery device, so every BLE link must be short and must always be closed.
    // Work is expressed as *pending operations* (read state / write set point / write mode /
    // sync clock), not as a queue of one-shot commands:
    //
    //   update()/control()  -> set pending flag(s) -> request a link
    //   link + PIN accepted -> read the device state (always, before any write)
    //                       -> write what is pending, packed from the state read IN THIS LINK
    //                          (mode, set point, clock, E10 acknowledgment - in that order)
    //                       -> re-read -> informational reads still due (after boot / daily)
    //                       -> close the link, disable the ble_client
    //   link lost half-way  -> the pending flags survive and are retried on a new link
    //                          (informational reads never cause or retry a link of their own)
    //
    // Retries: a fast phase (retry_window, back-off 3/15/30/60/120 s) followed by a slow phase
    // (every 5 min, after one hour every 15 min). A requested set point / mode is NEVER dropped on a
    // timer before COMMAND_TTL (24 h): an eTRV at the edge of the range gets it as soon as it is
    // reachable again. Only protocol errors that retrying cannot fix (PIN rejected, data that cannot
    // be decrypted = wrong secret_key, missing characteristics) give up after MAX_HARD_ERRORS.
    //
    // Safety nets (each one alone prevents a hung link that would drain the eTRV battery):
    //   * in-flight request counter reset on every link loss,
    //   * request timeout (no response within request_timeout -> link torn down),
    //   * link watchdog (any link older than LINK_WATCHDOG_MS is torn down),
    //   * client-state watchdog (ESPHome client stuck outside IDLE without a link -> reset).
    //
    // Links are requested through the ESP32BLETracker "promotion" path (client state DISCOVERED):
    // the tracker stops the scan, raises the WiFi/BT coexistence preference to Bluetooth and
    // serialises connection attempts - exactly what bluetooth_proxy does. A direct
    // BLEClientBase::connect() is only used as a fallback when the tracker does not promote us.
    // ------------------------------------------------------------------------------------------

    class Device : public MyComponent, public esphome::ble_client::BLEClientNode
    {
    public:
      Device() : xxtea(make_shared<Xxtea>()){};

      void dump_config() override;

      void setup() override;
      void loop() override;
      void update() override;
      void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) override;

      void set_secret_key(uint8_t *, bool) override;
      void publish_climate_state() override;

      void set_secret_key(const string &);
      void set_pin_code(const string &);
      void set_request_timeout(uint32_t ms) { this->request_timeout_ms_ = ms; }
      void set_retry_window(uint32_t ms) { this->retry_window_ms_ = ms; }
#ifdef USE_TIME
      void set_time_id(time::RealTimeClock *time) { this->time_ = time; }
#endif

    protected:
      void control(const ClimateCall &call) override;

      // --- transaction / link management ---
      void request_work_(bool user_command, bool first_write = false);
      void try_connect_();
      void issue_next_batch_();
      void finish_transaction_(const char *reason);
      void teardown_link_(const char *reason, bool keep_enabled);
      void link_failed_(const char *why);
      void hard_error_(const char *why);
      void give_up_(const char *reason);
      void drop_writes_(const char *reason, bool temperature = true, bool settings = true);
      void save_pending_();
      void load_pending_();
      void schedule_retry_(const char *why);
      bool send_read_(DeviceProperty *p);
      bool send_write_(WritableProperty *p, uint8_t *buff, uint16_t len);
      bool has_writes_() const { return this->pending_write_temperature_ || this->pending_write_settings_; }
      bool has_pending_() const { return this->pending_read_ || this->has_writes_() || this->pending_secret_key_ || this->pending_time_sync_ || this->pending_e10_ack_; }
      bool in_fast_phase_(uint32_t now) const { return (now - this->txn_started_ms_) < this->retry_window_ms_; }
      bool sibling_connecting_() const;
      void publish_connection_(bool connected);
      void evaluate_clock_();
      bool time_sync_enabled_() const;
      bool expected_clock_(int32_t &epoch, int32_t &offset);
      void publish_problems_();

      // --- informational reads (device information, name, PIN protection, schedule) ---
      uint16_t info_wanted_mask_() const;
      int info_index_(const DeviceProperty *p) const;
      bool issue_info_batch_();
      void publish_schedule_();

      void write_pin();
      void on_write_pin(esp_ble_gattc_cb_param_t::gattc_write_evt_param);
      bool on_link_ready_();
      void invalidate_handles_(const char *why);

      void on_read(esp_ble_gattc_cb_param_t::gattc_read_char_evt_param);
      void on_write(esp_ble_gattc_cb_param_t::gattc_write_evt_param);
      void on_batch_complete_();

      void log_link_(const char *where, int level);
      uint32_t link_age_ms_() const;

      shared_ptr<Xxtea> xxtea;

      shared_ptr<WritableProperty> p_pin{nullptr};
      shared_ptr<BatteryProperty> p_battery{nullptr};
      shared_ptr<TemperatureProperty> p_temperature{nullptr};
      shared_ptr<SettingsProperty> p_settings{nullptr};
      shared_ptr<ErrorsProperty> p_errors{nullptr};
      shared_ptr<TimeProperty> p_time{nullptr};
      shared_ptr<SecretKeyProperty> p_secret_key{nullptr};
      shared_ptr<PinSettingsProperty> p_pin_settings{nullptr};
      shared_ptr<NameProperty> p_name{nullptr};
      shared_ptr<ScheduleProperty> p_schedule[3]{};
      shared_ptr<InfoProperty> p_info[INFO_COUNT]{};

      set<shared_ptr<DeviceProperty>> properties;

    private:
      // Requested but undelivered set point / mode, kept across a reboot or OTA.
      struct PendingWrites
      {
        uint8_t flags; // bit 0: set point, bit 1: mode
        float target;
        uint8_t mode;
      } __attribute__((packed));
      ESPPreferenceObject pending_pref_;
      bool pending_saved_{false};
      ESPPreferenceObject secret_pref_;
      uint32_t pin_code_ = 0;
#ifdef USE_TIME
      time::RealTimeClock *time_{nullptr};
#endif

      // configuration
      uint32_t request_timeout_ms_{15000};
      uint32_t retry_window_ms_{600000};

      // pending operations (survive link loss; see the class comment for when they are dropped)
      bool pending_read_{false};
      bool pending_write_temperature_{false};
      bool pending_write_settings_{false};
      bool pending_secret_key_{false};
      bool pending_time_sync_{false};
      bool pending_e10_ack_{false};      // clear the latched E10 flag once the clock is right
      // Version of the requested value (bumped by control()) vs. the version a write carried: a
      // write only clears its pending flag if no newer value was requested while it was in flight.
      uint8_t temperature_seq_{0}, temperature_sent_seq_{0};
      uint8_t settings_seq_{0}, settings_sent_seq_{0};
      float requested_target_temperature_{NAN};
      ClimateMode requested_mode_{ClimateMode::CLIMATE_MODE_HEAT};
      uint32_t temperature_requested_ms_{0}; // when the pending set point was requested (TTL)
      uint32_t settings_requested_ms_{0};    // when the pending mode was requested (TTL)
      float target_before_request_{NAN};     // shown again if a request is dropped before any read
      ClimateMode mode_before_request_{ClimateMode::CLIMATE_MODE_HEAT};
      uint32_t last_read_ms_{0};             // last complete, plausible read of the device state
      bool read_once_{false};
      uint32_t last_time_sync_ms_{0};        // last clock write attempt (OK or not)
      bool time_sync_attempted_{false};
      uint32_t last_e10_ack_ms_{0};          // last E10 acknowledgment attempt (OK or not)
      bool e10_ack_attempted_{false};
      bool dst_warning_logged_{false};
      bool clock_set_once_{false};           // a clock write was accepted since boot
      bool clock_lost_{false};               // ... and a later read found the clock lost again
      uint16_t reported_error_flags_{0};
      bool reported_clock_lost_{false};
      bool errors_reported_{false};
      // informational reads still to do (bit = info item, see device.cpp); never a reason for a link
      uint16_t info_pending_{0};
      uint16_t info_batch_mask_{0};          // informational reads sent in the current batch
      uint32_t schedule_read_ms_{0};
      bool schedule_read_once_{false};

      // transaction state
      bool want_link_{false};            // we have pending work and are (re)trying to get a link
      uint32_t txn_started_ms_{0};       // start of the current fast phase
      bool slow_phase_{false};
      uint32_t failing_since_ms_{0};     // when the slow phase started
      uint32_t last_fast_rearm_ms_{0};
      uint8_t connect_attempts_{0};      // explicit link requests made in this transaction
      uint8_t open_failures_{0};         // failed opens of any origin (ours or auto_connect)
      uint8_t links_this_txn_{0};        // links actually established in this transaction
      uint8_t retry_count_{0};           // failures in this fast phase (back-off index)
      uint32_t next_connect_ms_{0};      // earliest time for the next connection request
      uint32_t connect_requested_ms_{0}; // when we set DISCOVERED (promotion fallback timer)
      bool boot_disable_done_{false};
      uint8_t consecutive_hard_errors_{0};

      // GATT handles: resolved by service discovery on the first link after boot, then reused on
      // every following link without discovery (ESPHome V3_WITH_CACHE client mode).
      bool handles_verified_{false};
      // The stack's (persistent) service table must be rebuilt before the next discovery.
      bool gatt_cache_clean_pending_{false};

      // current link state
      uint8_t batch_{0};                 // which operation batch the in-flight requests belong to
      uint8_t batch_link_errors_{0};     // responses with a transient/stack status in this batch
      uint8_t batch_hard_errors_{0};     // rejected / unusable responses in this batch
      bool fresh_read_this_link_{false}; // the device state was read successfully in this link
      bool clock_ok_this_link_{false};   // the eTRV clock is known to be right (read or written in this link)
      bool wrote_settings_this_link_{false};
      bool wrote_temperature_this_link_{false};
      bool link_up_{false};
      uint32_t link_started_ms_{0};
      uint8_t inflight_{0};              // GATT requests sent and not yet answered on this link
      uint32_t last_activity_ms_{0};     // last request sent / response received
      uint32_t parent_busy_since_ms_{0}; // client-state watchdog: when the client became busy
      bool parent_busy_{false};          // ... and whether that timer is armed
      ClientState last_parent_state_{ClientState::INIT};
      bool pin_inflight_{false};
      esp_gatt_status_t last_open_status_{ESP_GATT_OK};
      uint16_t last_open_conn_id_{0xFFFF};
      int8_t connected_published_{-1};

      // diagnostics
      ClientState last_node_state_{ClientState::INIT};
      uint32_t last_status_log_ms_{0};
    };

  } // namespace danfoss_eco
} // namespace esphome

#endif // USE_ESP32
