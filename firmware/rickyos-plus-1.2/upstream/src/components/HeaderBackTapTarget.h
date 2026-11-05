#pragma once

#include <algorithm>

// Tap rect of the back button BaseTheme::drawHeader paints on touch boards.
// The header's FreeInkUI frame is non-interactive, so the rect is recorded
// here at draw time and consumed by MappedInputManager's Back mapping; every
// screen that draws a titled header gets tap-to-go-back without its own
// routing. Cleared on activity exit (and by headerless draws) so a stale rect
// never eats taps on a new screen. Written by the render task and read by the
// loop task; a torn int read at worst misroutes one tap on a band that is
// being redrawn, so no lock is taken.
namespace HeaderBackTapTarget {
inline int x = 0;
inline int y = 0;
inline int w = 0;
inline int h = 0;

inline void set(const int newX, const int newY, const int newW, const int newH) {
  x = newX;
  y = newY;
  w = newW;
  h = newH;
}

// Thumb-sized target for a header's back arrow: the whole leading region (arrow
// plus title, the usual "tap the title to go back"), the full band height, and
// at least 160 px wide (~14 mm on a 300 ppi panel) but never past half the band,
// so trailing header buttons stay reachable. The drawn 48 px arrow alone is
// about 4 mm, too small to hit reliably.
inline void setLeading(const int bandX, const int bandY, const int bandWidth, const int bandHeight,
                       const int leadingWidth) {
  constexpr int MIN_WIDTH = 160;
  const int maxWidth = std::max(0, bandWidth / 2);
  set(bandX, bandY, std::clamp(leadingWidth, std::min(MIN_WIDTH, maxWidth), maxWidth), bandHeight);
}

inline void clear() { w = 0; }

inline bool contains(const int tx, const int ty) { return w > 0 && tx >= x && tx < x + w && ty >= y && ty < y + h; }
}  // namespace HeaderBackTapTarget
