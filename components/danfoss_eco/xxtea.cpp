#include "xxtea.h"
#include <cstring>

#define DELTA 0x9e3779b9
#define MX (((z>>5^y<<2) + (y>>3^z<<4)) ^ ((sum^y) + (key[(p&3)^e] ^ z)))

int Xxtea::set_key(uint8_t *key, size_t len)
{
    if (key == nullptr || len != 16) {
        return XXTEA_STATUS_NOT_INITIALIZED;
    }
    
    // Convert byte key to 32-bit words
    for (int i = 0; i < 4; i++) {
        key_[i] = ((uint32_t)key[i*4]) | 
                  ((uint32_t)key[i*4+1] << 8) |
                  ((uint32_t)key[i*4+2] << 16) |
                  ((uint32_t)key[i*4+3] << 24);
    }
    
    status_ = XXTEA_STATUS_SUCCESS;
    return status_;
}

void Xxtea::xxtea_encrypt(uint32_t *v, int n, uint32_t const key[4])
{
    uint32_t y, z, sum;
    unsigned p, rounds, e;
    
    if (n < 1) return;
    
    rounds = 6 + 52/n;
    sum = 0;
    z = v[n-1];
    
    do {
        sum += DELTA;
        e = (sum >> 2) & 3;
        for (p=0; p<n-1; p++) {
            y = v[p+1]; 
            z = v[p] += MX;
        }
        y = v[0];
        z = v[n-1] += MX;
    } while (--rounds);
}

void Xxtea::xxtea_decrypt(uint32_t *v, int n, uint32_t const key[4])
{
    uint32_t y, z, sum;
    unsigned p, rounds, e;
    
    if (n < 1) return;
    
    rounds = 6 + 52/n;
    sum = rounds*DELTA;
    y = v[0];
    
    do {
        e = (sum >> 2) & 3;
        for (p=n-1; p>0; p--) {
            z = v[p-1];
            y = v[p] -= MX;
        }
        z = v[n-1];
        y = v[0] -= MX;
        sum -= DELTA;
    } while (--rounds);
}

void Xxtea::encrypt(uint8_t *data, size_t len)
{
    if (status_ != XXTEA_STATUS_SUCCESS || data == nullptr || len == 0) {
        return;
    }
    
    // Ensure length is multiple of 4
    int n = (len + 3) / 4;
    xxtea_encrypt((uint32_t*)data, n, key_);
}

void Xxtea::decrypt(uint8_t *data, size_t len)
{
    if (status_ != XXTEA_STATUS_SUCCESS || data == nullptr || len == 0) {
        return;
    }
    
    // Ensure length is multiple of 4
    int n = (len + 3) / 4;
    xxtea_decrypt((uint32_t*)data, n, key_);
}
