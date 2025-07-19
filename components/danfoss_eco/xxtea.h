#pragma once

#include <cstdint>
#include <cstddef>

// key Size is always fixed
#define MAX_XXTEA_KEY8 16
// 32 Bit
#define MAX_XXTEA_KEY32 4

#define XXTEA_STATUS_NOT_INITIALIZED -1
#define XXTEA_STATUS_SUCCESS 0

// Standalone XXTEA implementation
class Xxtea
{
private:
    uint32_t key_[4];
    int status_;
    
    void xxtea_encrypt(uint32_t *v, int n, uint32_t const key[4]);
    void xxtea_decrypt(uint32_t *v, int n, uint32_t const key[4]);

public:
    Xxtea() : status_(XXTEA_STATUS_NOT_INITIALIZED) {}

    int set_key(uint8_t *key, size_t len);
    int status() const { return status_; }
    
    void encrypt(uint8_t *data, size_t len);
    void decrypt(uint8_t *data, size_t len);
};
