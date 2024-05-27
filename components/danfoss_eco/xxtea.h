#pragma once
#include <stdint.h>
#include <string.h>


#ifndef MAX_XXTEA_DATA8
// Maximum Size of Data Buffer in Bytes
#define MAX_XXTEA_DATA8  80
#endif


//!< All is well
#define XXTEA_STATUS_SUCCESS          0
//!< Generic Error in failure to execute
#define XXTEA_STATUS_GENERAL_ERROR    1
//!< Errors in input parameters
#define XXTEA_STATUS_PARAMETER_ERROR  2
//!< Error in Size of either of the buffers
#define XXTEA_STATUS_SIZE_ERROR       3
//!< Error in word alignment of the buffer
#define XXTEA_STATUS_ALIGNMENT_ERROR  4

// Find size of Uint32 array from an input of Byte array size
#define UINT32CALCBYTE(X) ( ( ( X & 3 ) != 0 ) ? ((X >> 2) + 1) : (X >> 2) )

// Find size of Byte array from an input of Uint32 array size
#define BYTECALCUINT32(X) ( X << 2 )


// key Size is always fixed
#define MAX_XXTEA_KEY8 16
// 32 Bit
#define MAX_XXTEA_KEY32 4
// DWORD Size of Data Buffer
#define MAX_XXTEA_DATA32 (UINT32CALCBYTE(MAX_XXTEA_DATA8))

#define XXTEA_STATUS_NOT_INITIALIZED -1





class Xxtea
{
public:
    Xxtea() : status_(XXTEA_STATUS_NOT_INITIALIZED){};

    int set_key(uint8_t *key, size_t len);

    int encrypt(uint8_t *data, size_t len, uint8_t *buf, size_t *maxlen);
    int decrypt(uint8_t *data, size_t len);

    int status() { return this->status_; }

private:
    int status_;
    uint32_t xxtea_data[MAX_XXTEA_DATA32];
    uint32_t xxtea_key[MAX_XXTEA_KEY32];
};