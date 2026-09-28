#pragma once

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include "my_component.h"
#include "device_data.h"

namespace esphome
{
    namespace danfoss_eco
    {
        using namespace std;
        using namespace esphome::esp32_ble_tracker;
        using namespace esphome::ble_client;

        // eTRV GATT layout (handles as found on firmware in the field; they are always resolved by
        // service discovery). See README "Bluetooth characteristics" for the complete list, including
        // the characteristics that are deliberately never touched (bootloader, update token, ...).
        static auto SERVICE_SETTINGS = ESPBTUUID::from_raw("10020000-2749-0001-0000-00805f9b042f");
        static auto CHARACTERISTIC_PIN = ESPBTUUID::from_raw("10020001-2749-0001-0000-00805f9b042f");          // 0x24
        static auto CHARACTERISTIC_PIN_SETTINGS = ESPBTUUID::from_raw("10020002-2749-0001-0000-00805f9b042f"); // 0x27
        static auto CHARACTERISTIC_SETTINGS = ESPBTUUID::from_raw("10020003-2749-0001-0000-00805f9b042f");     // 0x2a
        static auto CHARACTERISTIC_TEMPERATURE = ESPBTUUID::from_raw("10020005-2749-0001-0000-00805f9b042f");  // 0x2d
        static auto CHARACTERISTIC_NAME = ESPBTUUID::from_raw("10020006-2749-0001-0000-00805f9b042f");         // 0x30
        static auto CHARACTERISTIC_TIME = ESPBTUUID::from_raw("10020008-2749-0001-0000-00805f9b042f");         // 0x36
        static auto CHARACTERISTIC_ERRORS = ESPBTUUID::from_raw("10020009-2749-0001-0000-00805f9b042f");       // 0x39
        static auto CHARACTERISTIC_SECRET_KEY = ESPBTUUID::from_raw("1002000b-2749-0001-0000-00805f9b042f");   // 0x3f
        static auto CHARACTERISTIC_SCHEDULE_1 = ESPBTUUID::from_raw("1002000d-2749-0001-0000-00805f9b042f");   // 0x45
        static auto CHARACTERISTIC_SCHEDULE_2 = ESPBTUUID::from_raw("1002000e-2749-0001-0000-00805f9b042f");   // 0x48
        static auto CHARACTERISTIC_SCHEDULE_3 = ESPBTUUID::from_raw("1002000f-2749-0001-0000-00805f9b042f");   // 0x4b

        static auto SERVICE_BATTERY = ESPBTUUID::from_uint32(0x180F);
        static auto CHARACTERISTIC_BATTERY = ESPBTUUID::from_uint32(0x2A19); // 0x10

        // Device Information Service (plain text), read once after boot.
        static auto SERVICE_DEVICE_INFORMATION = ESPBTUUID::from_uint32(0x180A);

        const uint16_t INVALID_HANDLE = 0xFFFF;
        
        const uint8_t SECRET_KEY_LENGTH = 16;
        struct SecretKeyValue
        {
            SecretKeyValue() {}
            SecretKeyValue(uint8_t *val)
            {
                memcpy(this->value, (const char *)val, SECRET_KEY_LENGTH);
            }
            uint8_t value[SECRET_KEY_LENGTH];
        };

        class DeviceProperty
        {
        public:
            unique_ptr<DeviceData> data{nullptr};

            DeviceProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea, ESPBTUUID s_uuid, ESPBTUUID c_uuid) : component_(component), xxtea_(xxtea), service_uuid(s_uuid), characteristic_uuid(c_uuid) {}

            // Parses a read response. Returns false when the payload is unusable (decrypt failure or
            // implausible values - usually a wrong secret_key); `data` is then left untouched.
            virtual bool update_state(uint8_t *value, uint16_t value_len) { return true; }
            // Expected payload length of a successful read (0 = any length).
            virtual uint16_t expected_length() const { return 0; }
            bool accepts_length(uint16_t len) const { return this->expected_length() == 0 || len == this->expected_length(); }
            // Essential properties are part of every transaction: a failure there fails the link and
            // counts as a protocol error. Informational ones (device information, name, schedule ...)
            // never do - a failed information read is simply tried again on a later link.
            virtual bool essential() const { return true; }

            virtual bool init_handle(BLEClient *);
            bool read_request(BLEClient *client);
            bool has_handle() const { return this->handle != INVALID_HANDLE; }

            // GATT attribute handle, resolved on every connection in init_handle(). Always start
            // (and reset) to INVALID_HANDLE so a failed lookup can never leave a stale/garbage
            // handle behind that a later read/write would hit.
            uint16_t handle{INVALID_HANDLE};

        protected:
            shared_ptr<MyComponent> component_{nullptr};
            shared_ptr<Xxtea> xxtea_{nullptr};

            ESPBTUUID service_uuid;
            ESPBTUUID characteristic_uuid;
        };

        class WritableProperty : public DeviceProperty
        {
        public:
            WritableProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea, ESPBTUUID s_uuid, ESPBTUUID c_uuid) : DeviceProperty(component, xxtea, s_uuid, c_uuid) {}

            // log_data=false for plaintext secrets (the PIN): only encrypted payloads are logged
            bool write_request(BLEClient *client, uint8_t *data, uint16_t data_len, bool log_data = true);
        };

        class BatteryProperty : public DeviceProperty
        {
        public:
            BatteryProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : DeviceProperty(component, xxtea, SERVICE_BATTERY, CHARACTERISTIC_BATTERY) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 1; }
        };

        class TemperatureProperty : public WritableProperty
        {
        public:
            TemperatureProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : WritableProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_TEMPERATURE) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 8; }
            TemperatureData *get() const { return static_cast<TemperatureData *>(this->data.get()); }
        };

        class SettingsProperty : public WritableProperty
        {
        public:
            SettingsProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : WritableProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_SETTINGS) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 16; }
            SettingsData *get() const { return static_cast<SettingsData *>(this->data.get()); }
        };

        // Error flags. Written only to acknowledge (clear) E10 once the clock is right again.
        class ErrorsProperty : public WritableProperty
        {
        public:
            ErrorsProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : WritableProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_ERRORS) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 8; }
            ErrorsData *get() const { return static_cast<ErrorsData *>(this->data.get()); }
        };

        // Current time (optional, only used when a time source is configured).
        class TimeProperty : public WritableProperty
        {
        public:
            TimeProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : WritableProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_TIME) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 8; }
            TimeData *get() const { return static_cast<TimeData *>(this->data.get()); }
        };

        class SecretKeyProperty : public DeviceProperty
        {
        public:
            SecretKeyProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : DeviceProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_SECRET_KEY) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 16; }

            bool init_handle(BLEClient *) override;
        };

        // --- informational, read-only (never written by this component) ---

        // One Device Information Service string (manufacturer, model, serial, revisions).
        class InfoProperty : public DeviceProperty
        {
        public:
            InfoProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea, uint16_t uuid16, InfoField field, const char *label) : DeviceProperty(component, xxtea, SERVICE_DEVICE_INFORMATION, ESPBTUUID::from_uint16(uuid16)), field_(field), label_(label) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            bool essential() const override { return false; }
            const string &value() const { return this->value_; }

        protected:
            InfoField field_;
            const char *label_;
            string value_;
        };

        class NameProperty : public DeviceProperty
        {
        public:
            NameProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : DeviceProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_NAME) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            bool essential() const override { return false; }
        };

        class PinSettingsProperty : public DeviceProperty
        {
        public:
            PinSettingsProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea) : DeviceProperty(component, xxtea, SERVICE_SETTINGS, CHARACTERISTIC_PIN_SETTINGS) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return 8; }
            bool essential() const override { return false; }
        };

        // One of the three schedule characteristics; Device publishes the week once all three are read.
        class ScheduleProperty : public DeviceProperty
        {
        public:
            ScheduleProperty(shared_ptr<MyComponent> &component, shared_ptr<Xxtea> &xxtea, uint8_t part) : DeviceProperty(component, xxtea, SERVICE_SETTINGS, part == 0 ? CHARACTERISTIC_SCHEDULE_1 : (part == 1 ? CHARACTERISTIC_SCHEDULE_2 : CHARACTERISTIC_SCHEDULE_3)), part_(part) {}
            bool update_state(uint8_t *value, uint16_t value_len) override;
            uint16_t expected_length() const override { return this->part_ == 0 ? 20 : 12; }
            bool essential() const override { return false; }
            SchedulePartData *get() const { return static_cast<SchedulePartData *>(this->data.get()); }

        protected:
            uint8_t part_;
        };

    } // namespace danfoss_eco
} // namespace esphome
