#include "sha1.h"
#include <string.h>

#define SHA1_K0 0x5a827999
#define SHA1_K20 0x6ed9eba1
#define SHA1_K40 0x8f1bbcdc
#define SHA1_K60 0xca62c1d6

const uint8_t sha1InitState[] = {
  0x01, 0x23, 0x45, 0x67, // H0
  0x89, 0xab, 0xcd, 0xef, // H1
  0xfe, 0xdc, 0xba, 0x98, // H2
  0x76, 0x54, 0x32, 0x10, // H3
  0xf0, 0xe1, 0xd2, 0xc3  // H4
};

void Sha1Class::init(void) {
  memcpy(_state.b, sha1InitState, HASH_LENGTH);
  _byteCount = 0;
  _bufferOffset = 0;
}

uint32_t Sha1Class::rol32(uint32_t number, uint8_t bits) {
  return ((number << bits) | (number >> (32 - bits)));
}

void Sha1Class::hashBlock() {
  uint8_t i;
  uint32_t a, b, c, d, e, t;

  a = _state.w[0];
  b = _state.w[1];
  c = _state.w[2];
  d = _state.w[3];
  e = _state.w[4];
  
  for (i=0; i<80; i++) {
    if (i>=16) {
      t = _buffer.w[(i+13)&15] ^ _buffer.w[(i+8)&15] ^ _buffer.w[(i+2)&15] ^ _buffer.w[i&15];
      _buffer.w[i&15] = rol32(t, 1);
    }
    
    if (i<20) {
      t = (d ^ (b & (c ^ d))) + SHA1_K0;
    } else if (i<40) {
      t = (b ^ c ^ d) + SHA1_K20;
    } else if (i<60) {
      t = ((b & c) | (d & (b | c))) + SHA1_K40;
    } else {
      t = (b ^ c ^ d) + SHA1_K60;
    }
    
    t += rol32(a, 5) + e + _buffer.w[i&15];
    e = d;
    d = c;
    c = rol32(b, 30);
    b = a;
    a = t;
  }
  
  _state.w[0] += a;
  _state.w[1] += b;
  _state.w[2] += c;
  _state.w[3] += d;
  _state.w[4] += e;
}

void Sha1Class::addUncounted(uint8_t data) {
  _buffer.b[_bufferOffset ^ 3] = data;
  _bufferOffset++;
  if (_bufferOffset == BLOCK_LENGTH) {
    hashBlock();
    _bufferOffset = 0;
  }
}

size_t Sha1Class::write(uint8_t data) {
  ++_byteCount;
  addUncounted(data);
  return 1;
}

void Sha1Class::pad() {
  // Implement SHA-1 padding (fips180-2 §5.1.1)

  // Pad with 0x80 followed by 0x00 until the end of the block
  addUncounted(0x80);
  while (_bufferOffset != 56) addUncounted(0x00);

  // Append length in the last 8 bytes
  addUncounted(0); // We're only using 32 bit length info
  addUncounted(0); // for now...
  addUncounted(0); //
  addUncounted(_byteCount >> 29);
  addUncounted(_byteCount >> 21);
  addUncounted(_byteCount >> 13);
  addUncounted(_byteCount >> 5);
  addUncounted(_byteCount << 3);
}

uint8_t* Sha1Class::result(void) {
  // Pad to complete the last block
  pad();
  
  // Swap byte order back
  for (int i=0; i<5; i++) {
    uint32_t a, b;
    a = _state.w[i];
    b = a<<24;
    b |= (a<<8) & 0x00ff0000;
    b |= (a>>8) & 0x0000ff00;
    b |= a>>24;
    _state.w[i] = b;
  }
  
  return _state.b;
}

void Sha1Class::initHmac(const uint8_t* secret, int secretLength) {
  uint8_t i;
  memset(_keyBuffer, 0, BLOCK_LENGTH);
  
  if (secretLength > BLOCK_LENGTH) {
    // Hash long keys
    init();
    for (; secretLength--;) write(*secret++);
    memcpy(_keyBuffer, result(), HASH_LENGTH);
  } else {
    // Block pad short keys
    memcpy(_keyBuffer, secret, secretLength);
  }
  
  init();
  for (i=0; i<BLOCK_LENGTH; i++) {
    write(_keyBuffer[i] ^ 0x36);
  }
}

uint8_t* Sha1Class::resultHmac(void) {
  uint8_t i;
  
  // Complete inner hash
  memcpy(_innerHash, result(), HASH_LENGTH);
  
  init();
  for (i=0; i<BLOCK_LENGTH; i++) {
    write(_keyBuffer[i] ^ 0x5c);
  }
  for (i=0; i<HASH_LENGTH; i++) {
    write(_innerHash[i]);
  }
  
  return result();
}

Sha1Class Sha1;