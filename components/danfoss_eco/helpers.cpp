#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "helpers.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
namespace esphome
{
    namespace danfoss_eco
    {

        void encode_hex(const uint8_t *data, size_t len, char *buff)
        {
            for (size_t i = 0; i < len; i++)
                sprintf(buff + (i * 2), "%02x", data[i]);
        }

        optional<int> parse_hex(const char chr)
        {
            int out = chr;
            if (out >= '0' && out <= '9')
                return (out - '0');
            if (out >= 'A' && out <= 'F')
                return (10 + (out - 'A'));
            if (out >= 'a' && out <= 'f')
                return (10 + (out - 'a'));
            return {};
        }

        void parse_hex_str(const char *data, size_t str_len, uint8_t *buff)
        {
            size_t len = str_len / 2;
            for (size_t i = 0; i < len; i++)
                buff[i] = (parse_hex(data[i * 2]).value() << 4) | parse_hex(data[i * 2 + 1]).value();
        }

        uint32_t parse_int(uint8_t *data, int start_pos)
        {
            return int(data[start_pos] << 24 | data[start_pos + 1] << 16 | data[start_pos + 2] << 8 | data[start_pos + 3]);
        }

        uint16_t parse_short(uint8_t *data, int start_pos)
        {
            return short(data[start_pos] << 8 | data[start_pos + 1]);
        }

        void write_int(uint8_t *data, int start_pos, int value)
        {
            data[start_pos] = value >> 24;
            data[start_pos + 1] = value >> 16;
            data[start_pos + 2] = value >> 8;
            data[start_pos + 3] = value;
        }

        bool parse_bit(uint8_t data, int pos) { return (data & (1 << pos)) >> pos; }

        bool parse_bit(uint16_t data, int pos) { return (data & (1 << pos)) >> pos; }

        void set_bit(uint8_t data, int pos, bool value)
        {
            data ^= (-value ^ data) & (1UL << pos);
        }

        void reverse_chunks(uint8_t *data, int len, uint8_t *reversed_buff)
        {
            for (int i = 0; i < len; i += 4)
            {
                int l = MIN(4, len - i); // limit for a chunk, 4 or what's left
                for (int j = 0; j < l; j++)
                {
                    reversed_buff[i + j] = data[i + (l - 1 - j)];
                }
            }
        }

        uint8_t *encrypt(shared_ptr<Xxtea> &xxtea, uint8_t *value, uint16_t value_len)
        {
            // Log input data
            ESP_LOGV(TAG, "ENCRYPT INPUT[%d]: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len,
                     value_len > 0 ? value[0] : 0, value_len > 1 ? value[1] : 0,
                     value_len > 2 ? value[2] : 0, value_len > 3 ? value[3] : 0,
                     value_len > 4 ? value[4] : 0, value_len > 5 ? value[5] : 0,
                     value_len > 6 ? value[6] : 0, value_len > 7 ? value[7] : 0);
            
            uint8_t buffer[value_len];
            reverse_chunks(value, value_len, buffer);

            // Log after reverse_chunks
            ESP_LOGV(TAG, "AFTER REVERSE: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len > 0 ? buffer[0] : 0, value_len > 1 ? buffer[1] : 0,
                     value_len > 2 ? buffer[2] : 0, value_len > 3 ? buffer[3] : 0,
                     value_len > 4 ? buffer[4] : 0, value_len > 5 ? buffer[5] : 0,
                     value_len > 6 ? buffer[6] : 0, value_len > 7 ? buffer[7] : 0);

            xxtea->encrypt(buffer, value_len);
            
            // Log after encryption
            ESP_LOGV(TAG, "AFTER ENCRYPT: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len > 0 ? buffer[0] : 0, value_len > 1 ? buffer[1] : 0,
                     value_len > 2 ? buffer[2] : 0, value_len > 3 ? buffer[3] : 0,
                     value_len > 4 ? buffer[4] : 0, value_len > 5 ? buffer[5] : 0,
                     value_len > 6 ? buffer[6] : 0, value_len > 7 ? buffer[7] : 0);
            
            reverse_chunks(buffer, value_len, value);
            
            // Log final result
            ESP_LOGV(TAG, "ENCRYPT OUTPUT: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len > 0 ? value[0] : 0, value_len > 1 ? value[1] : 0,
                     value_len > 2 ? value[2] : 0, value_len > 3 ? value[3] : 0,
                     value_len > 4 ? value[4] : 0, value_len > 5 ? value[5] : 0,
                     value_len > 6 ? value[6] : 0, value_len > 7 ? value[7] : 0);
            
            return value;
        }

        uint8_t *decrypt(shared_ptr<Xxtea> &xxtea, uint8_t *value, uint16_t value_len)
        {
            // Log input data
            ESP_LOGV(TAG, "DECRYPT INPUT[%d]: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len,
                     value_len > 0 ? value[0] : 0, value_len > 1 ? value[1] : 0,
                     value_len > 2 ? value[2] : 0, value_len > 3 ? value[3] : 0,
                     value_len > 4 ? value[4] : 0, value_len > 5 ? value[5] : 0,
                     value_len > 6 ? value[6] : 0, value_len > 7 ? value[7] : 0);
            
            uint8_t buffer[value_len];
            reverse_chunks(value, value_len, buffer);

            // Log after reverse_chunks
            ESP_LOGV(TAG, "AFTER REVERSE: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len > 0 ? buffer[0] : 0, value_len > 1 ? buffer[1] : 0,
                     value_len > 2 ? buffer[2] : 0, value_len > 3 ? buffer[3] : 0,
                     value_len > 4 ? buffer[4] : 0, value_len > 5 ? buffer[5] : 0,
                     value_len > 6 ? buffer[6] : 0, value_len > 7 ? buffer[7] : 0);

            xxtea->decrypt(buffer, value_len);
            
            // Log after decryption
            ESP_LOGV(TAG, "AFTER DECRYPT: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len > 0 ? buffer[0] : 0, value_len > 1 ? buffer[1] : 0,
                     value_len > 2 ? buffer[2] : 0, value_len > 3 ? buffer[3] : 0,
                     value_len > 4 ? buffer[4] : 0, value_len > 5 ? buffer[5] : 0,
                     value_len > 6 ? buffer[6] : 0, value_len > 7 ? buffer[7] : 0);
            
            reverse_chunks(buffer, value_len, value);
            
            // Log final result
            ESP_LOGV(TAG, "DECRYPT OUTPUT: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     value_len > 0 ? value[0] : 0, value_len > 1 ? value[1] : 0,
                     value_len > 2 ? value[2] : 0, value_len > 3 ? value[3] : 0,
                     value_len > 4 ? value[4] : 0, value_len > 5 ? value[5] : 0,
                     value_len > 6 ? value[6] : 0, value_len > 7 ? value[7] : 0);
            
            return value;
        }

        void copy_address(uint64_t mac, esp_bd_addr_t bd_addr)
        {
            bd_addr[0] = (mac >> 40) & 0xFF;
            bd_addr[1] = (mac >> 32) & 0xFF;
            bd_addr[2] = (mac >> 24) & 0xFF;
            bd_addr[3] = (mac >> 16) & 0xFF;
            bd_addr[4] = (mac >> 8) & 0xFF;
            bd_addr[5] = (mac >> 0) & 0xFF;
        }

    }
}