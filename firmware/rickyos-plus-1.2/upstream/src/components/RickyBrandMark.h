#pragma once

#include <algorithm>
#include <cstdint>

#include "Rect.h"
#include "images/RickyCat.h"
#include "images/RickyDog.h"
#include "images/RickyLogo.h"

// User-selected portrait + dog + circular border + RickyOS wordmark.
// Immutable Flash pixels; reuse the existing framebuffer with no heap allocation.
namespace RickyBrandMark {
constexpr Rect fit(const Rect box) {
  const int height = std::max(
      0, std::min({box.height, RickyLogoAsset::height, box.width * RickyLogoAsset::height / RickyLogoAsset::width}));
  const int width = height * RickyLogoAsset::width / RickyLogoAsset::height;
  return Rect{box.x + (box.width - width) / 2, box.y + (box.height - height) / 2, width, height};
}

constexpr bool pixel(const int x, const int y, const uint8_t phase) {
  if (x < 0 || y < 0 || x >= RickyLogoAsset::width || y >= RickyLogoAsset::height) return false;
  const int index = y * (RickyLogoAsset::width / 8) + x / 8;
  const uint8_t mask = 0x80U >> (x % 8);
  if (phase == 0) return (RickyLogoAsset::circleBits[index] & mask) == 0;
  if ((RickyLogoAsset::bits[index] & mask) != 0) return false;
  if (phase >= 3 || (RickyLogoAsset::circleBits[index] & mask) == 0) return true;
  return y < RickyLogoAsset::wordmarkTop && (phase >= 2 || x < RickyLogoAsset::personRight);
}

// The portrait badge (person, dog and its own circular ring) occupies this
// square of the asset: the ring fits a circle centred near (192, 188) with an
// outer radius of 188 px. scripts/tests/test_rickyos_logo_asset.py re-derives it.
constexpr int badgeX = 4, badgeY = 0, badgeSize = 376;

// Area-sampled badge: each target pixel inks when at least `threshold`/16 of its
// source block is ink, so the thin ring stays an even line when shrunk.
template <typename Renderer>
void drawBadge(const Renderer& renderer, const Rect box, const int threshold = 7) {
  const int size = std::min(box.width, box.height);
  if (size <= 0) return;
  for (int y = 0; y < size; ++y) {
    const int y0 = badgeY + y * badgeSize / size;
    const int y1 = std::max(y0 + 1, badgeY + (y + 1) * badgeSize / size);
    for (int x = 0; x < size; ++x) {
      const int x0 = badgeX + x * badgeSize / size;
      const int x1 = std::max(x0 + 1, badgeX + (x + 1) * badgeSize / size);
      int ink = 0;
      for (int sy = y0; sy < y1 && sy < RickyLogoAsset::wordmarkTop; ++sy)
        for (int sx = x0; sx < x1; ++sx) ink += pixel(sx, sy, 3) ? 1 : 0;
      if (ink * 16 >= threshold * (y1 - y0) * (x1 - x0)) renderer.drawPixel(box.x + x, box.y + y, true);
    }
  }
}

// The home card's peeking companion: the approved isolated dog, or the cat with the cat theme.
struct PeekArt {
  int width, height;
  const uint8_t* coverage;
};
constexpr PeekArt dogPeek() { return {RickyDogAsset::width, RickyDogAsset::height, RickyDogAsset::coverage}; }
constexpr PeekArt catPeek() { return {RickyCatAsset::width, RickyCatAsset::height, RickyCatAsset::coverage}; }

// Use the approved isolated dog; cropping the badge also drew its circular ring.
// Card overdraw hides the lower edge. Paper pixels are transparent.
template <typename Renderer>
void drawDogPeek(const Renderer& renderer, const Rect box, const PeekArt art = dogPeek()) {
  if (box.width <= 0 || box.height <= 0) return;
  for (int y = 0; y < box.height; ++y) {
    const int sy = y * art.height / box.height;
    for (int x = 0; x < box.width; ++x) {
      const int sx = x * art.width / box.width;
      const int offset = sy * art.width + sx;
      const uint8_t packed = art.coverage[offset / 2];
      const int level = offset % 2 ? packed & 15 : packed >> 4;
      if (!level) continue;
      if (renderer.isGrayscale16Active())
        renderer.drawGrayscale16Pixel(box.x + x, box.y + y, 255 - level * 17);
      else if (level >= 8)
        renderer.drawPixel(box.x + x, box.y + y, true);
    }
  }
}

template <typename Renderer>
void draw(const Renderer& renderer, const Rect box, const uint8_t phase) {
  const Rect mark = fit(box);
  if (mark.width <= 0 || mark.height <= 0) return;
  for (int y = 0; y < mark.height; ++y) {
    const int sourceY = y * RickyLogoAsset::height / mark.height;
    for (int x = 0; x < mark.width; ++x) {
      if (pixel(x * RickyLogoAsset::width / mark.width, sourceY, phase)) {
        renderer.drawPixel(mark.x + x, mark.y + y, true);
      }
    }
  }
}
}  // namespace RickyBrandMark
