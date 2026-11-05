#pragma once
#include <cstdint>

// One lock-free 32-bit request: direction and normalized touch origin are
// consumed together by the render task, never as independently changing fields.
namespace nativePageTurn {
constexpr int normalize(int pixel, int extent) {
  return extent > 1 && pixel >= 0 && pixel < extent ? static_cast<int>(int64_t{pixel} * 1023 / (extent - 1)) : -1;
}
constexpr uint32_t pack(int direction, int x = -1, int y = -1) {
  if (direction != 1 && direction != -1) return 0;
  const uint32_t bits = direction > 0 ? 1 : 2;
  if (x < 0 || x > 1023 || y < 0 || y > 1023) return bits;
  return bits | 4u | (static_cast<uint32_t>(x) << 3) | (static_cast<uint32_t>(y) << 13);
}
constexpr int direction(uint32_t request) { return (request & 3) == 1 ? 1 : (request & 3) == 2 ? -1 : 0; }
constexpr int originX(uint32_t request) { return request & 4 ? static_cast<int>((request >> 3) & 1023) : -1; }
constexpr int originY(uint32_t request) { return request & 4 ? static_cast<int>((request >> 13) & 1023) : -1; }
}  // namespace nativePageTurn
