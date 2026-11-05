#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

// 阅读线: a quiet line under every text line. Drawn after the page, in every render
// pass: on the 16-level frame it is gray and never lightens a pixel (glyph ink that
// reaches into it keeps its tone); B/W passes draw the same shape in ink, except the
// highlight band, which would turn into a black bar and becomes a dotted line instead.
namespace readingGuideLine {

// Stored in settings: append only, never renumber.
enum class Style : uint8_t { Solid = 0, ShortDash, MediumDash, LongDash, Dotted, Wavy, Double, Highlight, Count };

constexpr int kStroke = 2;                   // stroke::kHairline
constexpr uint8_t kLineGray = 7 * 17;        // #777: quiet, and still ink in the B/W proxy (§3)
constexpr uint8_t kHighlightGray = 13 * 17;  // gray::kTint: a band, paper in B/W frames
constexpr int kWaveAmplitude = 2;
constexpr int kWavePeriod = 20;
constexpr int kDoubleGap = 2;         // paper between the two strokes of the double line
constexpr int kHighlightHeight = 10;  // band height; its bottom row sits one below the guide row

constexpr Style styleFor(const uint8_t rawStyle) {
  return rawStyle < static_cast<uint8_t>(Style::Count) ? static_cast<Style>(rawStyle) : Style::ShortDash;
}

// Rows a style paints above and below the guide row `y` (inclusive).
struct Extent {
  int up;
  int down;
  constexpr bool operator==(const Extent&) const = default;
};

constexpr Extent extentFor(const uint8_t rawStyle) {
  switch (styleFor(rawStyle)) {
    case Style::Wavy:
      return {kWaveAmplitude, kWaveAmplitude + kStroke - 1};
    case Style::Double:
      return {0, 2 * kStroke + kDoubleGap - 1};
    case Style::Highlight:
      return {kHighlightHeight - 2, 1};
    default:
      return {0, kStroke - 1};
  }
}

constexpr bool fitsVertically(const uint8_t rawStyle, const int y, const int top, const int bottomExclusive) {
  const Extent extent = extentFor(rawStyle);
  return y - extent.up >= top && y + extent.down < bottomExclusive;
}

// Offset from a text line's top to its guide row: just under this line's glyph bottoms,
// a small gap below them, so the text rests on the line like writing on ruled paper.
// The baseline sits `ascender` below the line top; a CJK ideograph spans about 0.80 em
// above it and 0.12 em below (measured on the reader faces). `em` is a CJK advance. The
// line never reaches the next line's glyph tops; tight spacing falls back to the row just
// under the glyphs.
// The reader face's em, measured on ideographs. A glyph the face lacks is measured in the
// (much smaller) UI fallback face instead, so a Traditional-only TTF measured 国 too small
// and every guide landed on the next row's glyph tops. 中 is in every CJK face; the largest
// of the three is the reader face's own.
template <typename Renderer>
int emFor(const Renderer& renderer, const int fontId) {
  int em = 0;
  for (const char* glyph : {"中", "国", "國"}) em = std::max(em, renderer.getTextWidth(fontId, glyph));
  return em;
}

constexpr int rowOffset(const int lineHeight, const int ascender, const int em) {
  const int glyphBottom = ascender + em * 12 / 100;
  const int nextGlyphTop = lineHeight + ascender - em * 80 / 100;
  const int gap = em * 10 / 100 > 3 ? em * 10 / 100 : 3;
  const int resting = glyphBottom + gap;
  const int lowest = nextGlyphTop - kStroke - 1;
  if (resting <= lowest) return resting;
  return lowest > glyphBottom ? lowest : glyphBottom + 1;
}

struct Pattern {
  int dashLength;
  int gapLength;

  constexpr bool operator==(const Pattern& other) const {
    return dashLength == other.dashLength && gapLength == other.gapLength;
  }
};

// Dash rhythm along the line, sized for a ~300 ppi panel; {0, 0} is continuous.
constexpr Pattern patternFor(const uint8_t rawStyle) {
  switch (styleFor(rawStyle)) {
    case Style::Solid:
    case Style::Double:
    case Style::Highlight:
      return {0, 0};
    case Style::ShortDash:
      return {8, 6};
    case Style::MediumDash:
      return {14, 8};
    case Style::LongDash:
      return {24, 10};
    case Style::Dotted:
      return {kStroke, 4};
    case Style::Wavy:
    case Style::Count:
      break;
  }
  return {8, 6};
}

// One period of the wave's vertical offset (a sampled sine, amplitude 2): calm enough
// to stay inside the gap at the default line spacing.
inline constexpr int8_t kWave[kWavePeriod] = {0, 1, 1, 2, 2, 2, 2, 2, 1, 1, 0, -1, -1, -2, -2, -2, -2, -2, -1, -1};

// The reader menu's choices: off, then one entry per look (the three stored dash
// lengths show as one 虚线). Maps both ways between a choice and the two settings.
inline constexpr int kMenuChoices = 7;
inline constexpr Style kMenuStyles[kMenuChoices] = {Style::Solid, Style::Solid,  Style::ShortDash, Style::Dotted,
                                                    Style::Wavy,  Style::Double, Style::Highlight};
constexpr int menuIndexFor(const bool enabled, const uint8_t rawStyle) {
  if (!enabled) return 0;
  switch (styleFor(rawStyle)) {
    case Style::Solid:
      return 1;
    case Style::ShortDash:
    case Style::MediumDash:
    case Style::LongDash:
      return 2;
    case Style::Dotted:
      return 3;
    case Style::Wavy:
      return 4;
    case Style::Double:
      return 5;
    case Style::Highlight:
      return 6;
    case Style::Count:
      break;
  }
  return 2;
}

namespace detail {
template <typename Renderer>
bool grayActive(Renderer& renderer) {
  if constexpr (requires { renderer.isGrayscale16Active(); }) {
    return renderer.isGrayscale16Active();
  } else {
    return false;
  }
}

// A gray pixel on the 16-level frame, never lighter than what is there; ink otherwise.
template <typename Renderer>
void plot(Renderer& renderer, const int x, const int y, const uint8_t gray, const bool grayFrame) {
  if constexpr (requires {
                  renderer.drawGrayscale16Pixel(x, y, gray);
                  renderer.grayscale16Level(x, y);
                }) {
    if (grayFrame) {
      if (renderer.grayscale16Level(x, y) * 17 > gray) renderer.drawGrayscale16Pixel(x, y, gray);
      return;
    }
  }
  renderer.drawPixel(x, y, true);
}

template <typename Renderer>
void stroke(Renderer& renderer, const int x, const int y, const uint8_t gray, const bool grayFrame) {
  for (int row = 0; row < kStroke; ++row) plot(renderer, x, y + row, gray, grayFrame);
}
}  // namespace detail

template <typename Renderer>
void draw(Renderer& renderer, int x1, const int y, int x2, const uint8_t rawStyle) {
  if (x2 < x1) std::swap(x1, x2);
  const bool grayFrame = detail::grayActive(renderer);
  const Style style = styleFor(rawStyle);

  if (style == Style::Highlight) {
    if (grayFrame) {
      for (int row = y - (kHighlightHeight - 2); row <= y + 1; ++row) {
        for (int x = x1; x <= x2; ++x) detail::plot(renderer, x, row, kHighlightGray, true);
      }
      return;
    }
    // A solid band would be a black bar in a B/W frame: draw it as dots instead.
    for (int x = x1; x <= x2; x += kStroke + 4) {
      for (int dx = 0; dx < kStroke && x + dx <= x2; ++dx) detail::stroke(renderer, x + dx, y, kLineGray, false);
    }
    return;
  }

  if (style == Style::Wavy) {
    int previous = y + kWave[0];
    for (int x = x1; x <= x2; ++x) {
      const int waveY = y + kWave[(x - x1) % kWavePeriod];
      // Join steps so the curve never breaks into separate dots.
      for (int row = std::min(previous, waveY); row <= std::max(previous, waveY); ++row) {
        detail::stroke(renderer, x, row, kLineGray, grayFrame);
      }
      previous = waveY;
    }
    return;
  }

  if (style == Style::Double) {
    for (int x = x1; x <= x2; ++x) {
      detail::stroke(renderer, x, y, kLineGray, grayFrame);
      detail::stroke(renderer, x, y + kStroke + kDoubleGap, kLineGray, grayFrame);
    }
    return;
  }

  const Pattern pattern = patternFor(rawStyle);
  for (int x = x1; x <= x2; ++x) {
    if (pattern.gapLength != 0 && (x - x1) % (pattern.dashLength + pattern.gapLength) >= pattern.dashLength) continue;
    detail::stroke(renderer, x, y, kLineGray, grayFrame);
  }
}

}  // namespace readingGuideLine
