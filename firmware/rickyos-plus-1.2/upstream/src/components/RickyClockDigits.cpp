#include "RickyClockDigits.h"

#include <GfxRenderer.h>
#include <Memory.h>

#include <algorithm>

#include "fonts/rickyClockDigits.h"

namespace RickyClockDigits {
namespace {
const ricky_clock_digits::Glyph* glyphFor(const char ch) {
  for (const auto& glyph : ricky_clock_digits::kGlyphs) {
    if (glyph.ch == ch) return &glyph;
  }
  return nullptr;
}

int cellWidth(const ricky_clock_digits::Glyph& glyph) {
  return glyph.ch == ':' ? glyph.advance : ricky_clock_digits::kCellWidth;
}
}  // namespace

int capHeight() { return ricky_clock_digits::kCapHeight; }

int width(const char* text) {
  int total = 0;
  for (const char* c = text; *c; ++c) {
    if (const auto* glyph = glyphFor(*c)) total += cellWidth(*glyph);
  }
  return total;
}

void draw(GfxRenderer& renderer, int x, const int yTop, const char* text) {
  for (const char* c = text; *c; ++c) {
    const auto* glyph = glyphFor(*c);
    if (!glyph) continue;
    const int originX = x + (cellWidth(*glyph) - glyph->advance) / 2 + glyph->left;
    // Runs of (length - 1) << 4 | coverage, row by row (see the generated header).
    const int total = glyph->width * glyph->height;
    int pixel = 0;
    for (int i = 0; i < glyph->rleBytes && pixel < total; ++i) {
      const int run = (glyph->rle[i] >> 4) + 1;
      const int ink = glyph->rle[i] & 0x0F;
      if (ink == 0) {
        pixel += run;
        continue;
      }
      for (int end = std::min(total, pixel + run); pixel < end; ++pixel) {
        const int px = originX + pixel % glyph->width;
        const int py = yTop + pixel / glyph->width;
        const int paper = renderer.grayscale16Level(px, py);
        const int level = paper - (paper * ink + 7) / 15;  // ink over the picture, toward black
        renderer.drawGrayscale16Pixel(px, py, static_cast<uint8_t>(level * 17));
      }
    }
    x += cellWidth(*glyph);
  }
}

int widthBw(const char* text, const int scalePercent) { return width(text) * scalePercent / 100; }

int capHeightBw(const int scalePercent) { return capHeight() * scalePercent / 100; }

void drawBw(GfxRenderer& renderer, int x, const int yTop, const char* text, const int scalePercent) {
  // One coverage buffer for the whole string, sized for the largest glyph. A digit is
  // ~10 KB of coverage, and allocating per glyph freed and immediately re-requested a
  // differently sized block five times per "HH:MM" render, which is the allocate/free
  // pattern that fragments this device's heap. peakAllocBytes is unchanged; only the
  // number of allocator round-trips and the block-size churn drop.
  size_t peakAllocBytes = 0;
  for (const auto& glyph : ricky_clock_digits::kGlyphs) {
    peakAllocBytes = std::max(peakAllocBytes, static_cast<size_t>(glyph.width) * glyph.height);
  }
  if (peakAllocBytes == 0) return;

  auto coverage = makeUniqueNoThrow<uint8_t[]>(peakAllocBytes);
  if (!coverage) return;

  for (const char* c = text; *c; ++c) {
    const auto* glyph = glyphFor(*c);
    if (!glyph) continue;
    const int w = glyph->width;
    const int h = glyph->height;
    // Reused across glyphs, so clear the previous glyph's ink before unpacking this one.
    std::fill_n(coverage.get(), static_cast<size_t>(w) * h, uint8_t{0});
    int pixel = 0;
    for (int i = 0; i < glyph->rleBytes && pixel < w * h; ++i) {
      const int run = (glyph->rle[i] >> 4) + 1;
      const uint8_t ink = glyph->rle[i] & 0x0F;
      for (int end = std::min(w * h, pixel + run); pixel < end; ++pixel) coverage[pixel] = ink;
    }
    const auto at = [&](const int gx, const int gy) -> int {
      return gx < 0 || gy < 0 || gx >= w || gy >= h ? 0 : coverage[gy * w + gx];
    };
    const int originX = x + ((cellWidth(*glyph) - glyph->advance) / 2 + glyph->left) * scalePercent / 100;
    const int outW = w * scalePercent / 100;
    const int outH = h * scalePercent / 100;
    for (int oy = 0; oy < outH; ++oy) {
      // Source position in 1/256 pixel, sampled at the output pixel's centre.
      const int sy = ((oy * 2 + 1) * 100 * 128) / scalePercent - 128;
      const int y0 = sy >> 8, fy = sy & 255;
      for (int ox = 0; ox < outW; ++ox) {
        const int sx = ((ox * 2 + 1) * 100 * 128) / scalePercent - 128;
        const int x0 = sx >> 8, fx = sx & 255;
        const int top = at(x0, y0) * (256 - fx) + at(x0 + 1, y0) * fx;
        const int bottom = at(x0, y0 + 1) * (256 - fx) + at(x0 + 1, y0 + 1) * fx;
        const int value = top * (256 - fy) + bottom * fy;  // coverage * 65536
        // Below half coverage: the Light digits read a touch heavier in pure B/W.
        if (value >= 5 * 65536) renderer.drawPixel(originX + ox, yTop + oy, true);
      }
    }
    x += cellWidth(*glyph) * scalePercent / 100;
  }
}

void fadeToPaper(GfxRenderer& renderer, const int left, const int top, const int width, const int fullAt,
                 const int bottom, const int strength) {
  if (bottom <= top || width <= 0) return;
  const int ramp = std::max(1, fullAt - top);
  for (int x = left; x < left + width; ++x) {
    for (int y = top; y < bottom; ++y) {
      // 0..strength of the way from the picture to paper; an ordered dither on the
      // fractional step keeps 16 levels from banding across a tall fade.
      const int alpha256 = y >= fullAt ? strength * 256 / 15 : (y - top) * strength * 256 / (15 * ramp);
      const int level = renderer.grayscale16Level(x, y);
      const int lifted = level * 256 + (15 - level) * alpha256;
      static constexpr uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
      const int threshold = kBayer4[y & 3][x & 3] * 16 + 8;
      const int out = std::min(15, lifted / 256 + ((lifted & 255) > threshold ? 1 : 0));
      if (out != level) renderer.drawGrayscale16Pixel(x, y, static_cast<uint8_t>(out * 17));
    }
  }
}

}  // namespace RickyClockDigits
