#include "RickyAaIcons.h"

#include <GfxRenderer.h>

#include "icons/rickyAaIcons.h"

namespace RickyAaIcons {
namespace {
uint32_t fnv1a(const uint8_t* bytes, const int size) {
  uint32_t hash = 0x811C9DC5u;
  for (int i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x01000193u;
  return hash;
}
}  // namespace

bool draw(const GfxRenderer& renderer, const uint8_t* bits, const int width, const int height, const int x, const int y,
          const bool ink) {
  if (!renderer.isGrayscale16Active() || !bits) return false;
  // The 1-bpp arrays are per translation unit, so the twin is found by content.
  const uint32_t hash = fnv1a(bits, (width + 7) / 8 * height);
  const ricky_aa_icons::Icon* twin = nullptr;
  for (const auto& icon : ricky_aa_icons::kIcons) {
    if (icon.bitsHash == hash && icon.width == width && icon.height == height) {
      twin = &icon;
      break;
    }
  }
  if (!twin) return false;
#if RICKYOS_PAINT_PROFILE
  const GfxRenderer::PaintScope profile(GfxRenderer::PaintKind::Image);
#endif
  const int target = ink ? 0 : 15;
  int pixel = 0;
  const int total = width * height;
  for (int i = 0; i < twin->rleBytes && pixel < total; ++i) {
    const int run = (twin->rle[i] >> 4) + 1;
    const int coverage = twin->rle[i] & 0x0F;
    for (int r = 0; r < run && pixel < total; ++r, ++pixel) {
      if (coverage == 0) continue;
      const int px = x + pixel % width;
      const int py = y + pixel / width;
      const int paper = renderer.grayscale16Level(px, py);
      const int delta = (target - paper) * coverage;  // over 15, rounded either way
      const int level = paper + (delta >= 0 ? (delta + 7) / 15 : -((7 - delta) / 15));
      renderer.drawGrayscale16Pixel(px, py, static_cast<uint8_t>(level * 17));
    }
  }
  return true;
}
}  // namespace RickyAaIcons
