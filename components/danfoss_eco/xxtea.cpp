#include "xxtea.h"

#define DELTA 0x9e3779b9
#define MX ((((z>>5)^(y<<2)) + ((y>>3)^(z<<4))) ^ ((sum^y) + (key[(p&3)^e] ^ z)))


void dtea_fn1(uint32_t *v, int32_t n, uint32_t const key[4])
{
  uint32_t y, z, sum;
  uint32_t p, rounds, e, an;
  an = n;
  if (n > 1)
  { // Coding Part
    rounds = 6 + 52/an;
    sum = 0;
    z = v[an-1];
    do {
      sum += DELTA;
      e = (sum >> 2) & 3;
      for (p=0; p<an-1; p++)
      {
        y = v[p+1];
        z = v[p] += MX;
      }
      y = v[0];
      z = v[an-1] += MX;
    } while (--rounds);
  } // End of Encoding
  else if (n < -1)
  {  // Decoding Part
    an = (uint32_t)-n;
    rounds = 6 + 52/an;
    sum = rounds*DELTA;
    y = v[0];
    do {
      e = (sum >> 2) & 3;
      for (p=an-1; p>0; p--)
      {
        z = v[p-1];
        y = v[p] -= MX;
      }
      z = v[an-1];
      y = v[0] -= MX;
      sum -= DELTA;
    } while (--rounds);
  } // End of Decoding
}



int Xxtea::set_key(uint8_t *key, size_t len)
{
    this->status_ = XXTEA_STATUS_GENERAL_ERROR;
    size_t osz;

    do
    {
        // Parameter Check
        if (key == NULL || len <= 0 || len > MAX_XXTEA_KEY8)
        {
            this->status_ = XXTEA_STATUS_PARAMETER_ERROR;
            break;
        }

        osz = UINT32CALCBYTE(len);

        // Check for Size Errors
        if (osz > MAX_XXTEA_KEY32)
        {
            this->status_ = XXTEA_STATUS_SIZE_ERROR;
            break;
        }

        // Clear the Key
        memset((void *)this->xxtea_key, 0, MAX_XXTEA_KEY8);

        // Copy the Key from Buffer
        memcpy((void *)this->xxtea_key, (const void *)key, len);

        // We have Success
        this->status_ = XXTEA_STATUS_SUCCESS;
    } while (0);

    return this->status_;
}

int Xxtea::encrypt(uint8_t *data, size_t len, uint8_t *buf, size_t *maxlen)
{
    int ret = XXTEA_STATUS_GENERAL_ERROR;
    int32_t l;
    do
    {
        if (data == NULL || len <= 0 || len > MAX_XXTEA_DATA8 ||
            buf == NULL || *maxlen <= 0 || *maxlen < len)
        {
            ret = XXTEA_STATUS_PARAMETER_ERROR;
            break;
        }
        // Calculate the Length needed for the 32bit Buffer
        l = UINT32CALCBYTE(len);

        // Check if More than expected space is needed
        if (l > MAX_XXTEA_DATA32 || *maxlen < (l * 4))
        {
            ret = XXTEA_STATUS_SIZE_ERROR;
            break;
        }

        // Clear the Data
        memset((void *)this->xxtea_data, 0, MAX_XXTEA_DATA8);

        // Copy the Data from Buffer
        memcpy((void *)this->xxtea_data, (const void *)data, len);

        // Perform Encryption - Change for Unsigned Handling while using memory Pointer (18th July 2017)
        dtea_fn1(this->xxtea_data, l, (const uint32_t *)this->xxtea_key);

        // Copy Encrypted Data back to buffer
        memcpy((void *)buf, (const void *)this->xxtea_data, (l * 4));

        // Assign the Length
        *maxlen = l * 4;

        ret = XXTEA_STATUS_SUCCESS;
    } while (0);
    return ret;
}

int Xxtea::decrypt(uint8_t *data, size_t len)
{
    int ret = XXTEA_STATUS_GENERAL_ERROR;
    int32_t l;
    do
    {
        if (data == NULL || len <= 0 || (len % 4) != 0)
        {
            ret = XXTEA_STATUS_PARAMETER_ERROR;
            break;
        }
        if (len > MAX_XXTEA_DATA8)
        {
            ret = XXTEA_STATUS_SIZE_ERROR;
            break;
        }
        // Copy the Data into Processing Array
        memset((void *)this->xxtea_data, 0, MAX_XXTEA_DATA8);
        memcpy((void *)this->xxtea_data, (const void *)data, len);
        // Get the Actual Size in 32bits - Negative for Decryption
        l = -((int32_t)len / 4); // - Change for Using correct Signed arithmetic (18th July 2017)
        // Performn Decryption - Change for Unsigned Handling while using memory Pointer (18th July 2017)
        dtea_fn1(this->xxtea_data, l, (const uint32_t *)this->xxtea_key);
        // Copy Encrypted Data back to buffer
        memcpy((void *)data, (const void *)this->xxtea_data, len);
        ret = XXTEA_STATUS_SUCCESS;
    } while (0);
    return ret;
}