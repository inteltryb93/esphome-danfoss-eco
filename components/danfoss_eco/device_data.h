#pragma once

#include <cstring>
#include <string>

#include "esphome/core/log.h"
#include "esphome/components/climate/climate_mode.h"

#include "helpers.h"
#include "xxtea.h"

namespace esphome
{
    namespace danfoss_eco
    {
        using namespace std;
        using namespace climate;

        // ------------------------------------------------------------------------------------------
        // Decrypted eTRV characteristic payloads.
        //
        // Every data object keeps the complete decrypted payload (`raw_`) and is only `valid` when the
        // payload had the expected length, decrypted successfully AND passed a plausibility check.
        // Invalid data is never stored by the properties and can therefore never be written back.
        //
        // Why the plausibility check matters: with a wrong (but syntactically valid) secret_key XXTEA
        // "decrypts" to random bytes without any error. Writing such a block back to the 16-byte
        // settings characteristic randomises the eTRV configuration (display flip, "valve not
        // installed" = the eTRV re-enters mounting mode, child lock, min/max 55 C ...) - exactly the
        // "mode switch resets the valve" reports of upstream issue #11, whose original cause was an
        // upstream bug that encrypted and wrote only 8 of the 16 settings bytes (fixed here).
        //
        // Layouts and meanings follow the official Danfoss Eco app (v1.5.0, CharacteristicReader /
        // CharacteristicWriter), see README "Bluetooth characteristics".
        // ------------------------------------------------------------------------------------------
        struct DeviceData
        {
            static constexpr uint16_t MAX_LENGTH = 20;

            uint16_t length;
            bool valid{false};
            uint8_t raw_[MAX_LENGTH]{0}; // decrypted payload

            DeviceData(uint16_t l, shared_ptr<Xxtea> &xxtea) : length(l), xxtea_(xxtea) {}
            virtual ~DeviceData() {}

            // Decrypts `value` (the payload as received) into raw_. False on any mismatch/failure.
            bool load_(uint8_t *value, uint16_t value_len)
            {
                if (value_len != this->length || this->length > MAX_LENGTH)
                    return false;
                uint8_t buf[MAX_LENGTH];
                memcpy(buf, value, value_len);
                if (!decrypt(this->xxtea_, buf, value_len))
                    return false;
                memcpy(this->raw_, buf, value_len);
                return true;
            }

            static bool temp_in(float t, float lo, float hi) { return t >= lo && t <= hi; }

        protected:
            shared_ptr<Xxtea> xxtea_;
        };

        struct WritableData : public DeviceData
        {
            // NOTE: the length must be passed through (upstream hard-coded 8 here, which truncated the
            // 16-byte settings write - the root cause of upstream issue #11).
            WritableData(uint16_t l, shared_ptr<Xxtea> &xxtea) : DeviceData(l, xxtea) {}

            // Encrypts `buff` (length bytes, already filled by the subclass) in place.
            bool encrypt_(uint8_t *buff) { return encrypt(this->xxtea_, buff, this->length); }
        };

        // 10020005 (0x2d), 8 bytes: [0] set point * 2, [1] room temperature * 2, [2..7] reserved.
        struct TemperatureData : public WritableData
        {
            float target_temperature{NAN};
            float room_temperature{NAN};

            TemperatureData(shared_ptr<Xxtea> &xxtea, uint8_t *raw_data, uint16_t value_len) : WritableData(8, xxtea)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                this->target_temperature = this->raw_[0] / 2.0f;
                this->room_temperature = this->raw_[1] / 2.0f;
                // A wrong secret_key yields uniformly random bytes; real values are in these ranges.
                this->valid = temp_in(this->target_temperature, 4.0f, 35.0f) && temp_in(this->room_temperature, 0.0f, 50.0f);
            }

            // Writes a new set point. All other bytes are sent back exactly as read (like libetrv and
            // danfoss-eco-ha do). `target` must already be validated (5..30, multiple of 0.5).
            bool pack(uint8_t *buff, float target)
            {
                memcpy(buff, this->raw_, this->length);
                buff[0] = (uint8_t)lroundf(target * 2.0f);
                return this->encrypt_(buff);
            }
        };

        // 10020003 (0x2a), 16 bytes: [0] configuration bits, [1] min*2, [2] max*2, [3] frost*2, [4] mode,
        // [5] vacation temperature*2, [6..9] vacation start, [10..13] vacation end (int32 BE, UTC epoch),
        // [14..15] reserved.
        struct SettingsData : public WritableData
        {
            // Mode byte as the app encodes it (Mode.fromInt / Mode.fromReturnInt): vacation and pause
            // also store the mode the eTRV returns to afterwards (odd value = back to the schedule).
            enum DeviceMode : uint8_t
            {
                MANUAL = 0,
                SCHEDULED = 1,
                VACATION_THEN_MANUAL = 2,
                VACATION_THEN_SCHEDULED = 3,
                PAUSE_THEN_MANUAL = 4, // "Pause": heating off, frost protection only
                PAUSE_THEN_SCHEDULED = 5,
            };
            static constexpr uint8_t MODE_MAX = PAUSE_THEN_SCHEDULED;

            // Byte 0 configuration bits (bit numbers as stored; the app bit-reverses the byte first).
            bool get_adaptable_regulation() const { return parse_bit(this->raw_[0], 0); }  // app: "Forecast"
            bool get_daylight_saving() const { return parse_bit(this->raw_[0], 1); }       // automatic summer time
            bool get_vertical_installation() const { return parse_bit(this->raw_[0], 2); } // thermostat orientation
            bool get_display_flip() const { return parse_bit(this->raw_[0], 3); }          // display orientation
            bool get_slow_regulation() const { return parse_bit(this->raw_[0], 4); }       // heat control "moderate"
            bool get_calibrated() const { return parse_bit(this->raw_[0], 5); }
            bool get_valve_installed() const { return parse_bit(this->raw_[0], 6); }       // "installed" (regulating)
            bool get_lock_control() const { return parse_bit(this->raw_[0], 7); }          // child lock

            uint8_t raw_mode{0};
            ClimateMode device_mode{ClimateMode::CLIMATE_MODE_HEAT};

            float temperature_min{NAN};
            float temperature_max{NAN};
            float frost_protection_temperature{NAN};
            float vacation_temperature{NAN};
            // A vacation is planned when vacation_to lies in the future (the app ignores older ones).
            int32_t vacation_from{0}; // UTC epoch
            int32_t vacation_to{0};   // UTC epoch

            SettingsData(shared_ptr<Xxtea> &xxtea, uint8_t *raw_data, uint16_t value_len) : WritableData(16, xxtea)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                const uint8_t *settings = this->raw_;
                this->temperature_min = settings[1] / 2.0f;
                this->temperature_max = settings[2] / 2.0f;
                this->frost_protection_temperature = settings[3] / 2.0f;
                this->raw_mode = settings[4];
                this->vacation_temperature = settings[5] / 2.0f;
                this->vacation_from = (int32_t)parse_int(this->raw_, 6);
                this->vacation_to = (int32_t)parse_int(this->raw_, 10);

                // Plausibility: the settings block is the one we may write back, so be strict.
                const bool mode_ok = this->raw_mode <= MODE_MAX;
                this->valid = mode_ok &&
                              temp_in(this->temperature_min, 4.0f, 30.0f) &&
                              temp_in(this->temperature_max, 4.0f, 35.0f) &&
                              this->temperature_min <= this->temperature_max &&
                              temp_in(this->frost_protection_temperature, 4.0f, 30.0f);
                if (mode_ok)
                    this->device_mode = to_climate_mode(this->raw_mode);
            }

            bool is_paused() const { return this->raw_mode == PAUSE_THEN_MANUAL || this->raw_mode == PAUSE_THEN_SCHEDULED; }

            // HEAT = manual set point, AUTO = the eTRV decides (weekly schedule or vacation),
            // OFF = paused (frost protection only).
            static ClimateMode to_climate_mode(uint8_t mode)
            {
                switch (mode)
                {
                case SCHEDULED:
                case VACATION_THEN_MANUAL:
                case VACATION_THEN_SCHEDULED:
                    return ClimateMode::CLIMATE_MODE_AUTO;
                case PAUSE_THEN_MANUAL:
                case PAUSE_THEN_SCHEDULED:
                    return ClimateMode::CLIMATE_MODE_OFF;
                case MANUAL:
                default:
                    return ClimateMode::CLIMATE_MODE_HEAT;
                }
            }

            static const char *mode_str(uint8_t mode)
            {
                switch (mode)
                {
                case MANUAL:
                    return "MANUAL";
                case SCHEDULED:
                    return "SCHEDULED";
                case VACATION_THEN_MANUAL:
                    return "VACATION_THEN_MANUAL";
                case VACATION_THEN_SCHEDULED:
                    return "VACATION_THEN_SCHEDULED";
                case PAUSE_THEN_MANUAL:
                    return "PAUSE_THEN_MANUAL";
                case PAUSE_THEN_SCHEDULED:
                    return "PAUSE_THEN_SCHEDULED";
                default:
                    return "UNKNOWN";
                }
            }

            // Value of the `device_mode` text sensor.
            static const char *mode_name(uint8_t mode)
            {
                switch (mode)
                {
                case MANUAL:
                    return "manual";
                case SCHEDULED:
                    return "schedule";
                case VACATION_THEN_MANUAL:
                case VACATION_THEN_SCHEDULED:
                    return "vacation";
                case PAUSE_THEN_MANUAL:
                case PAUSE_THEN_SCHEDULED:
                    return "pause";
                default:
                    return "unknown";
                }
            }

            // The raw mode byte to write for the requested climate mode. A mode that already maps to
            // the requested climate mode is kept unchanged (a vacation stays a vacation for AUTO, a
            // pause stays a pause for OFF), so a request that does not change what HA shows never
            // rewrites byte 4. A new pause returns to the schedule if the eTRV runs it now (as the app
            // does).
            uint8_t raw_mode_for(ClimateMode mode) const
            {
                if (to_climate_mode(this->raw_mode) == mode)
                    return this->raw_mode;
                switch (mode)
                {
                case ClimateMode::CLIMATE_MODE_AUTO:
                    return SCHEDULED;
                case ClimateMode::CLIMATE_MODE_OFF:
                    return (this->raw_mode & 1) ? (uint8_t)PAUSE_THEN_SCHEDULED : (uint8_t)PAUSE_THEN_MANUAL;
                default:
                    return MANUAL;
                }
            }

            // Writes a new mode. ONLY byte 4 changes; the other 15 bytes are sent back exactly as they
            // were read (in the same BLE link, see Device::issue_next_batch_).
            bool pack(uint8_t *buff, ClimateMode mode)
            {
                memcpy(buff, this->raw_, this->length);
                buff[4] = this->raw_mode_for(mode);
                return this->encrypt_(buff);
            }
        };

        // 10020009 (0x39), 8 bytes: uint16 BE error flags at [0..1], [2..7] zero. Bit n is error code
        // E(n+1) (BufferedThermostat.setErrorFlags). The flags latch: the eTRV sets them, the app clears
        // one by writing the flags back without it (writeAlertsToThermostat), e.g. E10 after a battery
        // change once the clock has been set again.
        struct ErrorsData : public WritableData
        {
            static constexpr uint8_t E10_INVALID_TIME_BIT = 9;

            uint16_t flags{0};
            bool E9_VALVE_DOES_NOT_CLOSE{false};
            bool E10_INVALID_TIME{false};
            bool E14_LOW_BATTERY{false};
            bool E15_VERY_LOW_BATTERY{false};

            ErrorsData(shared_ptr<Xxtea> &xxtea, uint8_t *raw_data, uint16_t value_len) : WritableData(8, xxtea)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                this->flags = parse_short(this->raw_, 0);
                E9_VALVE_DOES_NOT_CLOSE = this->has_code(9);
                E10_INVALID_TIME = this->has_code(10);
                E14_LOW_BATTERY = this->has_code(14);
                E15_VERY_LOW_BATTERY = this->has_code(15);
                this->valid = true;
            }

            bool has_code(uint8_t code) const { return code >= 1 && code <= 16 && ((this->flags >> (code - 1)) & 1) != 0; }
            // any of E1..E5, E7, E8, E11..E13, E16 (sensor, memory, hardware, radio, encoder faults)
            bool hardware_fault() const { return (this->flags & 0x9CDF) != 0; }
            // bytes 2..7 as the app writes them (all zero); anything else is a layout we do not know
            bool tail_is_zero() const
            {
                for (uint16_t i = 2; i < this->length; i++)
                    if (this->raw_[i] != 0)
                        return false;
                return true;
            }

            // The app's format: uint16 BE flags, rest zero.
            bool pack(uint8_t *buff, uint16_t new_flags)
            {
                memset(buff, 0, this->length);
                buff[0] = new_flags >> 8;
                buff[1] = new_flags & 0xFF;
                return this->encrypt_(buff);
            }

            // Short English description (the four upstream strings are kept unchanged).
            static const char *title(uint8_t code)
            {
                switch (code)
                {
                case 1:
                    return "Front Sensor Error (E1)";
                case 2:
                    return "Valve Sensor Error (E2)";
                case 3:
                    return "Memory Error (E3)";
                case 4:
                    return "Hardware Error (E4)";
                case 5:
                    return "Error E5";
                case 6:
                    return "Motor Error (E6)";
                case 7:
                    return "Communication Module Error (E7)";
                case 8:
                    return "Invalid Communication (E8)";
                case 9:
                    return "Valve Stuck";
                case 10:
                    return "Invalid Time";
                case 11:
                    return "Error E11";
                case 12:
                    return "Radio Error (E12)";
                case 13:
                    return "Encoder Jammed (E13)";
                case 14:
                    return "Low Battery";
                case 15:
                    return "Very Low Battery";
                default:
                    return "Error E16";
                }
            }
        };

        // 10020008 (0x36), 8 bytes: int32 BE UTC epoch, int32 BE offset to local time [s]. The app
        // writes Date.getTime()/1000 and TimeZone.getRawOffset()/1000 (the standard-time offset): with
        // its automatic summer time flag (settings bit 1) the eTRV adds the DST hour itself.
        struct TimeData : public WritableData
        {
            int32_t epoch{0};
            int32_t utc_offset{0};

            TimeData(shared_ptr<Xxtea> &xxtea, uint8_t *raw_data, uint16_t value_len) : WritableData(8, xxtea)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                this->epoch = (int32_t)parse_int(this->raw_, 0);
                this->utc_offset = (int32_t)parse_int(this->raw_, 4);
                // any value is "valid": a wrong clock/offset is exactly what a sync corrects
                this->valid = true;
            }

            bool pack(uint8_t *buff, int32_t epoch, int32_t utc_offset)
            {
                memset(buff, 0, this->length);
                write_int(buff, 0, epoch);
                write_int(buff, 4, utc_offset);
                return this->encrypt_(buff);
            }
        };

        // 10020006 (0x30): the name given in the Danfoss app, UTF-8, NUL padded (15 bytes, encrypted
        // as 16).
        struct NameData : public DeviceData
        {
            string name;

            NameData(shared_ptr<Xxtea> &xxtea, uint8_t *raw_data, uint16_t value_len) : DeviceData(value_len, xxtea)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                size_t n = 0;
                while (n < value_len && this->raw_[n] != 0)
                    n++;
                for (size_t i = 0; i < n; i++)
                {
                    const uint8_t c = this->raw_[i];
                    this->name.push_back((c < 0x20 || c == 0x7F) ? ' ' : (char)c);
                }
                const size_t first = this->name.find_first_not_of(' ');
                const size_t last = this->name.find_last_not_of(' ');
                this->name = first == string::npos ? string() : this->name.substr(first, last - first + 1);
                this->valid = true;
            }
        };

        // 10020002 (0x27, "thermostat code"): [0..3] the PIN (uint32 BE), [4] PIN protection enabled.
        // Only the flag is kept; the PIN is wiped right after decryption and never logged.
        struct PinSettingsData : public DeviceData
        {
            bool pin_enabled{false};

            PinSettingsData(shared_ptr<Xxtea> &xxtea, uint8_t *raw_data, uint16_t value_len) : DeviceData(8, xxtea)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                this->pin_enabled = this->raw_[4] != 0;
                memset(this->raw_, 0, sizeof(this->raw_));
                this->valid = true;
            }
        };

        // Weekly program, split over three characteristics (CharacteristicReader.parseSchedule):
        //   1002000D (0x45, 20 bytes): [0] "at home" temperature *2, [1] "away" temperature *2, Mon, Tue, Wed
        //   1002000E (0x48, 12 bytes): Thu, Fri
        //   1002000F (0x4b, 12 bytes): Sat, Sun
        // Every day is 6 bytes: three "at home" periods (start, end) in half hours (0..48).
        struct SchedulePartData : public DeviceData
        {
            static constexpr uint8_t DAY_BYTES = 6;
            uint8_t part;

            SchedulePartData(shared_ptr<Xxtea> &xxtea, uint8_t part, uint8_t *raw_data, uint16_t value_len) : DeviceData(part == 0 ? 20 : 12, xxtea), part(part)
            {
                if (!this->load_(raw_data, value_len))
                    return;
                for (uint16_t i = this->days_offset(); i < this->length; i++)
                {
                    if (this->raw_[i] > 48)
                        return; // not a half-hour index: unknown layout or wrong key
                }
                if (part == 0 && (this->raw_[0] > 70 || this->raw_[1] > 70))
                    return;
                this->valid = true;
            }

            uint8_t days_offset() const { return this->part == 0 ? 2 : 0; }
            uint8_t day_count() const { return this->part == 0 ? 3 : 2; }
            uint8_t first_day() const { return this->part == 0 ? 0 : (this->part == 1 ? 3 : 5); } // 0 = Monday
            const uint8_t *day(uint8_t i) const { return this->raw_ + this->days_offset() + DAY_BYTES * i; }
            float home_temperature() const { return this->raw_[0] / 2.0f; }
            float away_temperature() const { return this->raw_[1] / 2.0f; }
        };

    } // namespace danfoss_eco
} // namespace esphome
