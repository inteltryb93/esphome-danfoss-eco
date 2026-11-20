#pragma once

#include "esphome/core/helpers.h"

#include "xxtea.h"

#include <esp_bt_defs.h>

#include <string>

namespace esphome
{
    namespace danfoss_eco
    {
        using namespace std;

        const char *const TAG = "danfoss_eco";

        // Hex dump of a short payload for a log line ("0A.1B.2C"), kept on the stack. Use it as a
        // temporary: HexDump(data, len).c_str(). It replaces ESPHome's std::string
        // format_hex_pretty(), which left esphome/core/helpers.h in ESPHome 2026.11.0. Payloads here
        // are at most 20 bytes; anything longer than the buffer is cut, never overrun.
        class HexDump
        {
        public:
            HexDump(const uint8_t *data, size_t len) { format_hex_pretty_to(this->buf_, data, len, '.'); }
            const char *c_str() const { return this->buf_; }

        private:
            char buf_[format_hex_pretty_size(32)];
        };

        void encode_hex(const uint8_t *data, size_t len, char *buff);
        bool parse_hex_str(const char *data, size_t str_len, uint8_t *buff);
        uint32_t parse_int(uint8_t *data, int start_pos);
        uint16_t parse_short(uint8_t *data, int start_pos);
        void write_int(uint8_t *data, int start_pos, int value);

        bool parse_bit(uint8_t data, int pos);
        bool parse_bit(uint16_t data, int pos);

        void reverse_chunks(uint8_t *data, int len, uint8_t *reversed_buff);

        bool encrypt(shared_ptr<Xxtea> &xxtea, uint8_t *value, uint16_t value_len);
        bool decrypt(shared_ptr<Xxtea> &xxtea, uint8_t *value, uint16_t value_len);

        void copy_address(uint64_t, esp_bd_addr_t);

        // Text read from the eTRV as valid UTF-8 (the API sends text sensor states as protobuf
        // strings; invalid UTF-8 makes Home Assistant drop the whole connection): an incomplete
        // sequence at the end (text cut at the characteristic's length) is dropped, any other
        // invalid byte becomes '?'.
        string sanitize_utf8(const string &in);
    }
}