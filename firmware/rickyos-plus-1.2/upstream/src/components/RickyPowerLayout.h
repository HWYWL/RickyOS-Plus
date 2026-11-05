#pragma once

#include <algorithm>

#include "Rect.h"
#include "RickyBrandMark.h"

// Safe, aspect-preserving geometry for the approved embedded logo and text.
namespace RickyPowerLayout {
struct Layout {
  Rect mark;
  Rect message;
  Rect footer;
};

constexpr Layout fit(const Rect safe, const int textHeight, const int gap) {
  const int padding = std::max(12, gap);
  const int footerHeight = textHeight * 2;
  const int available = std::max(0, safe.height - footerHeight - padding * 3);
  const Rect mark =
      RickyBrandMark::fit(Rect{0, 0, safe.width * 3 / 5, std::max(0, available - padding * 2 - textHeight * 2)});
  const int blockHeight = mark.height + padding * 2 + textHeight * 2;
  const int top = safe.y + std::max(0, (available - blockHeight) / 2);
  return {Rect{safe.x + (safe.width - mark.width) / 2, top, mark.width, mark.height},
          Rect{safe.x, top + mark.height + padding * 2, safe.width, textHeight * 2},
          Rect{safe.x, safe.y + safe.height - footerHeight - padding, safe.width, footerHeight}};
}
}  // namespace RickyPowerLayout
