#pragma once
#include <cstdint>
#include <cstring>
namespace recorder_wav {
constexpr uint32_t rate = 16000;
constexpr uint32_t maxBytes = rate * 2 * 60 * 60;  // one hour; bounded RIFF size
inline void put32(uint8_t* p, uint32_t n) {
  for (int i = 0; i < 4; ++i) p[i] = n >> (i * 8);
}
inline uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void header(uint8_t* out, uint32_t bytes) {
  memset(out, 0, 44);
  memcpy(out, "RIFF", 4);
  put32(out + 4, bytes + 36);
  memcpy(out + 8, "WAVEfmt ", 8);
  put32(out + 16, 16);
  out[20] = 1;
  out[22] = 1;
  put32(out + 24, rate);
  put32(out + 28, rate * 2);
  out[32] = 2;
  out[34] = 16;
  memcpy(out + 36, "data", 4);
  put32(out + 40, bytes);
}
// This recorder owns canonical PCM WAVs; external/music WAV parsing is separate.
inline bool valid(const uint8_t* h, uint64_t fileBytes) {
  uint8_t expected[44];
  const uint32_t n = get32(h + 40);
  header(expected, n);
  return n && n <= maxBytes && !(n % 2) && fileBytes == uint64_t(n) + 44 && memcmp(h, expected, 44) == 0;
}
}  // namespace recorder_wav
