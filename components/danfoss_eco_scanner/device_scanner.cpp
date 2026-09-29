#include "esphome/core/log.h"

#include "device_scanner.h"

#ifdef USE_ESP32

namespace esphome
{
    namespace danfoss_eco_scanner
    {
        static const string eTRV_SUFFIX = string(";eTRV");

        void DanfossEcoScanner::dump_config()
        {
            ESP_LOGCONFIG(TAG, "Danfoss Eco Scanner:");
            ESP_LOGCONFIG(TAG, "  Read Secret: %d", this->read_secret_);
        }

        bool DanfossEcoScanner::parse_device(const ESPBTDevice &device)
        {
            string name = device.get_name();
            int s_len = eTRV_SUFFIX.length();

            if (name.length() <= s_len || name.compare(name.length() - s_len, s_len, eTRV_SUFFIX) != 0)
                return false;

            // the first character of the name is a digit whose bit 2 is set while the pairing window is
            // open (hardware button pressed): the secret key can be read now
            const uint8_t flags = (uint8_t)name.c_str()[0];
            const bool ready = (flags & 0x4) != 0;
            const uint64_t address = device.address_uint64();
            auto it = this->seen_.find(address);
            if (it != this->seen_.end() && it->second == ready)
                return true; // already reported in this state
            if (it != this->seen_.end() || this->seen_.size() < MAX_SEEN)
                this->seen_[address] = ready;

            // address_str() is deprecated since ESPHome 2026.8.0 (removed in 2027.2.0)
            char addr[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
            device.address_str_to(addr);
            if (it == this->seen_.end())
                ESP_LOGI(TAG, "Found Danfoss eTRV, MAC: %s, Name: %s, RSSI: %d dBm", addr, name.c_str(), device.get_rssi());
            if (ready)
                ESP_LOGI(TAG, "%s: ready to read the secret key (pairing window open)", addr);
            else if (it != this->seen_.end())
                ESP_LOGI(TAG, "%s: pairing window closed", addr);

            return true;
        }

    } // namespace danfoss_eco_scanner
} // namespace esphome

#endif
