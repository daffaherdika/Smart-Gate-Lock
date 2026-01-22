#include "TOTP.h"

TOTP::TOTP(uint8_t* hmacKey, int keyLength, int timeStep) {
  _hmacKey = hmacKey;
  _keyLength = keyLength;
  _timeStep = timeStep;
}

char* TOTP::getCode(long timeStamp) {
  long steps = timeStamp / _timeStep;
  
  // Konversi Long ke Byte Array (Big Endian) yang BENAR dan AMAN
  // Loop terbalik dari 7 ke 0
  for(int i = 7; i >= 0; i--) {
    _byteArray[i] = steps & 0xFF;
    steps >>= 8;
  }

  Sha1.initHmac(_hmacKey, _keyLength);
  
  // Tulis 8 byte ke SHA1
  for(int i = 0; i < 8; i++) {
    Sha1.write(_byteArray[i]);
  }
  
  uint8_t* hash = Sha1.resultHmac();

  // Truncate (Ambil 4 byte hasil hash)
  int offset = hash[20 - 1] & 0xF;
  long truncatedHash = 0;
  for (int i = 0; i < 4; ++i) {
    truncatedHash <<= 8;
    truncatedHash |= hash[offset + i];
  }
  
  truncatedHash &= 0x7FFFFFFF;
  truncatedHash %= 1000000;

  sprintf(_code, "%06ld", truncatedHash);
  return _code;
}