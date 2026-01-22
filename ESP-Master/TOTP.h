#ifndef TOTP_h
#define TOTP_h
#include "Arduino.h"
#include "sha1.h"

class TOTP {
  public:
    TOTP(uint8_t* hmacKey, int keyLength, int timeStep = 30);
    char* getCode(long timeStamp);
  private:
    uint8_t* _hmacKey;
    int _keyLength;
    int _timeStep;
    uint8_t _byteArray[8];
    char _code[7];
};
#endif