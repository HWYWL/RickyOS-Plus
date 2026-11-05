#pragma once
#include <cstdint>
namespace plus_board {
// Legacy, uncalibrated cold-start fallback ONLY. The board estimator replaces
// this after a qualified full/loaded reference; never treat it as an OCV curve.
constexpr bool batteryPercentFromMv(uint16_t mv, uint16_t& percent) {
  if (mv < 2500 || mv > 5000) return false;
  percent = mv <= 3300 ? 0 : mv >= 4330 ? 100 : ((mv - 3300) * 100u + 515u) / 1030u;
  return true;
}
}  // namespace plus_board
