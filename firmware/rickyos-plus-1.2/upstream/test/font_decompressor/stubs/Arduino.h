#pragma once

#include <chrono>
#include <cstdint>

inline uint32_t micros() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
inline uint32_t millis() { return micros() / 1000; }
