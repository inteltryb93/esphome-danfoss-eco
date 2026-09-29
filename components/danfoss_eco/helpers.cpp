#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "helpers.h"

// ESP-IDF's <sys/param.h> already defines MIN on some targets (e.g. ESP32-C3/riscv), so guard it
// to avoid a "MIN redefined" warning.
#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif
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

        bool parse_hex_str(const char *data, size_t str_len, uint8_t *buff)
        {
            // Never call optional::value() on an invalid character: with -fno-exceptions that aborts
            // the firmware at boot (boot loop). climate.py validates the string, this is defence in depth.
            size_t len = str_len / 2;
            bool ok = true;
            for (size_t i = 0; i < len; i++)
            {
                auto hi = parse_hex(data[i * 2]);
                auto lo = parse_hex(data[i * 2 + 1]);
                if (!hi.has_value() || !lo.has_value())
                    ok = false;
                buff[i] = (hi.value_or(0) << 4) | lo.value_or(0);
            }
            return ok;
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

        // XXTEA as used by the eTRV: every 4-byte chunk is byte-reversed before and after the cipher.
        // Both functions work in place and return false (leaving `value` untouched) on any failure, so a
        // failed cipher can never be mistaken for plaintext (decrypt) or sent as plaintext (encrypt).
        static constexpr uint16_t MAX_CIPHER_LEN = 20;
        static bool cipher_len_ok(uint16_t value_len)
        {
            // eTRV characteristics are 8, 12 (schedule 2/3), 16 or 20 (schedule 1) bytes; XXTEA needs
            // >= 2 32-bit words.
            return value_len >= 8 && value_len <= MAX_CIPHER_LEN && (value_len % 4) == 0;
        }

        bool encrypt(shared_ptr<Xxtea> &xxtea, uint8_t *value, uint16_t value_len)
        {
            if (!cipher_len_ok(value_len))
            {
                ESP_LOGE(TAG, "encrypt: refusing length %u", (unsigned)value_len);
                return false;
            }
            uint8_t buffer[MAX_CIPHER_LEN], enc_buff[MAX_CIPHER_LEN];
            reverse_chunks(value, value_len, buffer);

            // Xxtea::encrypt() takes `size_t *maxlen` and writes `*maxlen = l * 4` back through it, so it
            // must point at a real size_t (the original code passed a uint16_t -> stack corruption).
            size_t out_len = value_len;
            auto xxtea_status = xxtea->encrypt(buffer, value_len, enc_buff, &out_len);
            if (xxtea_status != XXTEA_STATUS_SUCCESS || out_len != value_len)
            {
                ESP_LOGE(TAG, "xxtea_encrypt failed, len=%u out_len=%u status=%d", (unsigned)value_len, (unsigned)out_len, xxtea_status);
                return false;
            }
            reverse_chunks(enc_buff, value_len, value);
            return true;
        }

        bool decrypt(shared_ptr<Xxtea> &xxtea, uint8_t *value, uint16_t value_len)
        {
            if (!cipher_len_ok(value_len))
            {
                ESP_LOGW(TAG, "decrypt: unexpected length %u", (unsigned)value_len);
                return false;
            }
            uint8_t buffer[MAX_CIPHER_LEN];
            reverse_chunks(value, value_len, buffer);
            auto xxtea_status = xxtea->decrypt(buffer, value_len);
            if (xxtea_status != XXTEA_STATUS_SUCCESS)
            {
                ESP_LOGW(TAG, "xxtea_decrypt failed, len=%u status=%d", (unsigned)value_len, xxtea_status);
                return false;
            }
            reverse_chunks(buffer, value_len, value);
            return true;
        }

        string sanitize_utf8(const string &in)
        {
            string out;
            out.reserve(in.size());
            const size_t n = in.size();
            size_t i = 0;
            while (i < n)
            {
                const uint8_t c = (uint8_t)in[i];
                size_t len;
                uint32_t min_cp;
                if (c < 0x80)
                {
                    out.push_back((char)c);
                    i++;
                    continue;
                }
                else if ((c & 0xE0) == 0xC0)
                    len = 2, min_cp = 0x80;
                else if ((c & 0xF0) == 0xE0)
                    len = 3, min_cp = 0x800;
                else if ((c & 0xF8) == 0xF0)
                    len = 4, min_cp = 0x10000;
                else
                {
                    out.push_back('?'); // stray continuation byte or 0xF8..0xFF
                    i++;
                    continue;
                }
                bool ok = true;
                uint32_t cp = c & (0xFF >> (len + 1));
                size_t k = 1;
                for (; k < len && i + k < n; k++)
                {
                    const uint8_t cc = (uint8_t)in[i + k];
                    if ((cc & 0xC0) != 0x80)
                    {
                        ok = false;
                        break;
                    }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if (ok && k < len)
                    break; // incomplete sequence at the end: drop it
                if (!ok || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                {
                    out.push_back('?');
                    i++;
                    continue;
                }
                out.append(in, i, len);
                i += len;
            }
            return out;
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