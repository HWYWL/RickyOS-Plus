#pragma once

#include <algorithm>

namespace ricky::battery {
// Status fonts are registered built-in numeric faces. Use a fixed digit/%
// sample so 9 -> 10 -> 100 never moves the icon, and font leading is excluded.
template <class Renderer>
int iconTop(const Renderer& renderer, int fontId, int textY, int iconHeight) {
  const auto& fonts = renderer.getFontMap();
  const auto face = fonts.find(fontId);
  if (face == fonts.end()) return textY + (renderer.getLineHeight(fontId) - iconHeight) / 2;
  const auto* data = face->second.getData();
  int top = renderer.getLineHeight(fontId), bottom = 0;
  for (const char* digit = "0123456789%"; *digit; ++digit) {
    const auto* glyph = face->second.getGlyph(static_cast<unsigned char>(*digit));
    if (!glyph || !glyph->width || !glyph->height) continue;
    const int inkTop = data->ascender - glyph->top;
    top = std::min(top, inkTop);
    bottom = std::max(bottom, inkTop + glyph->height);
  }
  if (bottom <= top) return textY + (renderer.getLineHeight(fontId) - iconHeight) / 2;
  return textY + (top + bottom - iconHeight) / 2;
}
}  // namespace ricky::battery
