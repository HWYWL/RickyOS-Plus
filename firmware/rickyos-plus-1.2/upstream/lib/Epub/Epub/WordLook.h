#pragma once

#include <cstdint>

// How a word looks in a typeset book, beyond bold/italic/decoration (EpdFontFamily::Style):
// the book's own face, its text tone and (later) its size. Carried per word through layout
// and the section cache; 0 means "as the reader is set", so books without styles cost nothing.
//
//   bits  0- 7  face  : 0 = the reader font, n = slot n of the open book's BookFontSet
//   bits  8-11  ink   : text tone, 0 = black .. 15 = white (GfxRenderer::TextInkScope levels)
//   bits 12-15  reserved
//   bits 16-31  size  : 0 = the reader size, otherwise em * 256 relative to it
namespace wordlook {

inline constexpr uint32_t kDefault = 0;

constexpr uint8_t face(const uint32_t look) { return static_cast<uint8_t>(look & 0xFF); }
constexpr uint8_t ink(const uint32_t look) { return static_cast<uint8_t>((look >> 8) & 0x0F); }
constexpr uint16_t size(const uint32_t look) { return static_cast<uint16_t>(look >> 16); }

constexpr uint32_t make(const uint8_t face, const uint8_t ink, const uint16_t size = 0) {
  return static_cast<uint32_t>(face) | (static_cast<uint32_t>(ink & 0x0F) << 8) | (static_cast<uint32_t>(size) << 16);
}

// CSS luminance (0 = black .. 255 = white) to an ink level.
//   - Dark colors (#222, #333, navy, maroon: luma < 96) are body text meant as black; on a
//     reflective panel any lift reads as faded type, so they stay pure black.
//   - Clear grays (#666 .. #aaa) keep a visible tone, compressed to ink 3..8 so they remain
//     readable at small sizes.
//   - Near-white text is drawn black: it only reads on a colored background, and
//     backgrounds are not painted.
constexpr uint8_t inkForLuma(const uint8_t luma) {
  if (luma < 96 || luma >= 200) return 0;
  return static_cast<uint8_t>(3 + (luma - 96) * 5 / (200 - 96));
}

}  // namespace wordlook
