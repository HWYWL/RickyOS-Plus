#pragma once
#if __has_include(<BoardConfig.h>)
#include <BoardConfig.h>
#endif
#include <cstdint>

#include "FirstLineIndent.h"

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every field: a section file built
// with a different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which
// fills settings and viewport fields. The reader supplies collectTouchLinks
// from the input device and applies any session-only CSS fallback. Taking
// the viewport as arguments is what keeps a spec from
// existing in a half-filled state — the 0 defaults below are a last-resort
// backstop (a 0x0 viewport lays out nothing), not an invitation to omit it.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  uint8_t extraParagraphSpacing = 0;  // 0=off, 1..5=0.5x/0.75x/1x/1.25x/1.5x line height
  // Reader-level first-line indent, one of FirstLineIndent::Auto/Indent/
  // NoIndent (see ParsedText.h). Independent of extraParagraphSpacing:
  // paragraphs keep their indent even when extra paragraph spacing is
  // enabled.
  uint8_t firstLineIndent = FirstLineIndent::Auto;
  uint8_t paragraphIndentSpaces = 3;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  bool collectTouchLinks = false;
  // Lines are laid out as columns (VerticalText.h): the caller swaps the viewport so
  // viewportWidth is the column height and viewportHeight the width the columns fill.
  bool verticalText = false;
  // Fingerprint of the book's usable fonts (BookFontSet); 0 = none or the book's styles off.
  // Pagination measured words in those faces, so a change re-lays the chapter out.
  uint32_t bookFontsKey = 0;
#if FREEINK_DEVICE_METALIO_PLUS || FREEINK_DEVICE_READPICO
  // Chapter art can extend into the horizontal text margins, never into a
  // decorated reading frame. Part of the Plus section-cache layout key.
  uint16_t imageOutset = 0;
#endif

  bool operator==(const ReaderRenderSpec& other) const {
#if FREEINK_DEVICE_METALIO_PLUS || FREEINK_DEVICE_READPICO
    if (imageOutset != other.imageOutset) return false;
#endif
    return fontId == other.fontId && lineCompression == other.lineCompression &&
           extraParagraphSpacing == other.extraParagraphSpacing && firstLineIndent == other.firstLineIndent &&
           paragraphIndentSpaces == other.paragraphIndentSpaces && characterSpacing == other.characterSpacing &&
           wordSpacingPercent == other.wordSpacingPercent && paragraphAlignment == other.paragraphAlignment &&
           viewportWidth == other.viewportWidth && viewportHeight == other.viewportHeight &&
           hyphenationEnabled == other.hyphenationEnabled && embeddedStyle == other.embeddedStyle &&
           imageRendering == other.imageRendering && focusReadingEnabled == other.focusReadingEnabled &&
           collectTouchLinks == other.collectTouchLinks && verticalText == other.verticalText &&
           bookFontsKey == other.bookFontsKey;
  }
};
