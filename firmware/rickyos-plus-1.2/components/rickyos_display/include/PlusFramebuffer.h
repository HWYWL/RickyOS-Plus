#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace freeink::plus {
// Native gray begin starts a complete new page, rather than an overlay on
// the previous monochrome frame. Reuse the existing panel allocation.
inline void clearGrayscale(uint8_t* frame, size_t bytes) {
  memset(frame, 0xff, bytes);
}
// Native scan order is unchanged. The SDK supplies MSB-first 1bpp (white=1);
// EPDiy consumes even pixels in the low nibble, with white=15. Caller owns
// both buffers; the destination must have four times sourceBytes storage.
inline void expandMonochrome(const uint8_t* source, size_t sourceBytes, uint8_t* destination) {
  for (size_t i = 0; i < sourceBytes; ++i) {
    const uint8_t value = source[i];
    for (unsigned pair = 0; pair < 4; ++pair) {
      const uint8_t low = value & (0x80u >> (pair * 2)) ? 15 : 0;
      const uint8_t high = value & (0x40u >> (pair * 2)) ? 0xf0 : 0;
      destination[i * 4 + pair] = low | high;
    }
  }
}

// Legacy overlay selectors are NOT monochrome pages: (LSB, MSB) = 11 is
// dark gray, 01 light gray, 00 keeps the B/W base. Reuse the native front
// buffer to retain the LSB marker, with no additional full-screen allocation.
// The caller must provide an already committed B/W base (levels 0 / 15).
inline void stageOverlayLsb(const uint8_t* base, const uint8_t* lsb, size_t sourceBytes, uint8_t* frame) {
  for (size_t i = 0; i < sourceBytes; ++i) {
    for (unsigned pair = 0; pair < 4; ++pair) {
      uint8_t value = base[i * 4 + pair];
      if (lsb[i] & (0x80u >> (pair * 2))) value = (value & 0xf0) | 5;
      if (lsb[i] & (0x40u >> (pair * 2))) value = (value & 0x0f) | 0x50;
      frame[i * 4 + pair] = value;
    }
  }
}

inline void finishOverlayMsb(const uint8_t* base, const uint8_t* msb, size_t sourceBytes, uint8_t* frame) {
  for (size_t i = 0; i < sourceBytes; ++i) {
    for (unsigned pair = 0; pair < 4; ++pair) {
      const uint8_t staged = frame[i * 4 + pair];
      const uint8_t original = base[i * 4 + pair];
      const uint8_t low = (msb[i] & (0x80u >> (pair * 2))) ? ((staged & 15) == 5 ? 5 : 10) : original & 15;
      const uint8_t high = (msb[i] & (0x40u >> (pair * 2))) ? ((staged >> 4) == 5 ? 5 : 10) : original >> 4;
      frame[i * 4 + pair] = low | (high << 4);
    }
  }
}
}
