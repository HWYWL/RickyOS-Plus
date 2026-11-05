#pragma once
#include <cstdint>

class GfxRenderer;

// Anti-aliased twins of the RickyOS 1-bpp icons (src/components/icons/rickyAaIcons.h),
// drawn while a screen is rendered into the 16-level frame (a settled UI screen).
namespace RickyAaIcons {
// Draws the twin of `bits` (a w x h, MSB-first, 0 = ink icon) at (x, y) and returns true;
// returns false, drawing nothing, when no 16-level frame is active or no twin exists,
// so the caller draws the 1-bpp icon as before. `ink` false draws it white (on a pill).
bool draw(const GfxRenderer& renderer, const uint8_t* bits, int width, int height, int x, int y, bool ink);
}  // namespace RickyAaIcons
