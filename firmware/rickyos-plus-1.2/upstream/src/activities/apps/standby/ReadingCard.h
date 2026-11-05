#pragma once

#include <string>

#include "components/themes/BaseTheme.h"  // for ::Rect
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalMemory.h>
#endif

class GfxRenderer;

// "上次读到这里": the book-cover standby face as a card — the opening of the page the
// reader stopped on, with the cover, title, progress and (optionally) the time. Two
// layouts (CrossPointSettings::STANDBY_COVER_LAYOUT): an editorial page with the
// excerpt as its hero, and a split with the cover over the text.
//
// Drawn either into a 16-gray frame (cover in native gray; text goes through the B/W
// buffer and is copied in, as WallpaperFace's corner is) or plain B/W.
class ReadingCard {
 public:
  // False when there is no book to show (the face then falls back to the picture).
  bool load();
  bool drawGray(GfxRenderer& renderer, const Rect& viewport) const;
  void drawBw(GfxRenderer& renderer, const Rect& viewport) const;
  // Frees the display-size faces the card set its text in (call when the face leaves).
  void release();

 private:
  std::string coverPath_;  // fitted (uncropped) cover BMP, "" when the book has none
  std::string title_;
  std::string author_;
  std::string chapter_;
  std::string excerpt_;
  int progress_ = 0;
  unsigned todayMinutes_ = 0;
  // The text is set in display sizes of the reader's vector font, built on first draw
  // and kept while the face is up (a redraw each minute would otherwise rebuild them).
  mutable GfxRenderer* fontsOwner_ = nullptr;
#if FREEINK_DEVICE_METALIO_PLUS
  // The Plus decodes the book's own cover JPEG at full resolution for the card (the cached
  // cover BMP of a progressive JPEG holds only its 1/8 preview), scaled once to the box it
  // is drawn in and kept while the face is up. Only this card uses it.
  std::string sourceJpeg_;
  mutable HalMemory::PsramBuffer sharp_;
  mutable int sharpWidth_ = 0, sharpHeight_ = 0, sharpBoxWidth_ = 0, sharpBoxHeight_ = 0;
  mutable bool sharpFill_ = false, sharpFailed_ = false;
#endif
  // Draws the full-resolution cover into a 16-gray frame (Plus); false to use the BMP.
  bool drawSharpCover(GfxRenderer& renderer, int x, int y, int width, int height, bool fill) const;
  bool draw(GfxRenderer& renderer, const Rect& viewport, bool gray) const;
};
