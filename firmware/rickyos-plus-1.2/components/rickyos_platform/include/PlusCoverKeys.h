#pragma once
#include <cstdint>
namespace plus_board {
enum class CoverKey : uint8_t { None, Home, Previous, Next };
// OEM FT6336U cover coordinates, deliberately evaluated before screen mapping.
constexpr CoverKey coverKeyAt(uint16_t x, uint16_t y) {
  if (y < 1570 || y > 1630) return CoverKey::None;
  if (x >= 70 && x <= 130) return CoverKey::Home;
  if (x >= 170 && x <= 230) return CoverKey::Previous;
  if (x >= 270 && x <= 330) return CoverKey::Next;
  return CoverKey::None;
}
}
