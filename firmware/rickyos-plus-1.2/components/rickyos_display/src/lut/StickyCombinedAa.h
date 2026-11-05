#pragma once

#include <cstdint>

namespace freeink::stickyCombinedAa {
// Experimental Sticky-only calibration. 5 ms/frame (Paper Mono's rate 0x08).
// Gray is one intermediate shade; these defaults need on-glass acceptance.
inline constexpr uint8_t KICK_FRAMES = 16;
inline constexpr uint8_t GRAY_FRAMES = 24;
inline constexpr uint8_t BLACK_FRAMES = 32;
// Match lut_grayscale_sticky: VGH, VSH1, VSH2, VSL, VCOM.
inline constexpr uint8_t VOLTAGES[] = {0x17, 0x41, 0xA8, 0x32, 0x30};
static_assert(GRAY_FRAMES % 3 == 0 && KICK_FRAMES >= 12 && BLACK_FRAMES >= KICK_FRAMES + 12);
}  // namespace freeink::stickyCombinedAa
