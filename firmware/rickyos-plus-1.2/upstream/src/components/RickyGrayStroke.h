#pragma once

#include <GfxRenderer.h>

#include <algorithm>

#include "RickyDesignTokens.h"

// A rounded-rectangle outline in an exact 16-gray level. On a 16-gray frame only the
// ring's pixels are written (straight sides row by row in panel memory, the four
// corners per pixel), so whatever is inside stays; on a B/W frame it falls back to
// the spec's 1-bit ink. No allocation.
namespace RickyGrayStroke {
inline void ring(const GfxRenderer& renderer, int x, int y, int width, int height, int thickness, int radius,
                 ricky::ds::gray::Level level) {
  if (width <= 0 || height <= 0 || thickness <= 0) return;
  if (!renderer.isGrayscale16Active() || level == ricky::ds::gray::kInk) {
    renderer.drawRoundedRect(x, y, width, height, thickness, radius, ricky::ds::gray::oneBitInk(level));
    return;
  }
  radius = std::max(0, std::min({radius, width / 2, height / 2}));
  const int inner = std::max(0, radius - thickness);
  // Sides, between the corners.
  renderer.fillGrayscale16Rect(x + radius, y, width - 2 * radius, thickness, level);
  renderer.fillGrayscale16Rect(x + radius, y + height - thickness, width - 2 * radius, thickness, level);
  renderer.fillGrayscale16Rect(x, y + radius, thickness, height - 2 * radius, level);
  renderer.fillGrayscale16Rect(x + width - thickness, y + radius, thickness, height - 2 * radius, level);
  if (radius == 0) return;
  const uint8_t tone = ricky::ds::gray::toRgb8(level);
  for (int dy = 0; dy < radius; ++dy) {
    for (int dx = 0; dx < radius; ++dx) {
      // Distance from the corner circle's centre, sampled at the pixel centre.
      const int cx = radius - dx, cy = radius - dy;
      const int d2 = (2 * cx - 1) * (2 * cx - 1) + (2 * cy - 1) * (2 * cy - 1);
      if (d2 > 4 * radius * radius || d2 < 4 * inner * inner) continue;
      renderer.drawGrayscale16Pixel(x + dx, y + dy, tone);
      renderer.drawGrayscale16Pixel(x + width - 1 - dx, y + dy, tone);
      renderer.drawGrayscale16Pixel(x + dx, y + height - 1 - dy, tone);
      renderer.drawGrayscale16Pixel(x + width - 1 - dx, y + height - 1 - dy, tone);
    }
  }
}
}  // namespace RickyGrayStroke
