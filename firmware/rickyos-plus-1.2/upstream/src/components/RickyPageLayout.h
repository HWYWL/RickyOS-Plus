#pragma once
#include <algorithm>

#include "Rect.h"

// One geometry source for drawn cards and their hit regions. No panel constants.
namespace RickyPageLayout {
struct NavigationRows {
  int iconY;
  int labelY;
};
inline NavigationRows navigationRows(const Rect& bar, int iconSize, int labelHeight) {
  constexpr int gap = 6;
  const int padding = std::max(4, (bar.height - iconSize - gap - labelHeight) / 2);
  return {bar.y + padding, bar.y + padding + iconSize + gap};
}
inline int columns(const Rect& body, int count) {
  return body.width > body.height ? (count == 4 ? 4 : std::min(count, 3)) : std::min(count, 2);
}
inline Rect cell(const Rect& body, int slot, int count, int gap, int requestedColumns = 0) {
  if (count <= 0 || slot < 0 || slot >= count) return Rect{};
  const int cols = requestedColumns > 0 ? std::min(count, requestedColumns) : columns(body, count);
  const int rows = (count + cols - 1) / cols;
  const int width = std::max(0, (body.width - gap * (cols - 1)) / cols);
  const int height = std::max(0, (body.height - gap * (rows - 1)) / rows);
  return Rect{body.x + slot % cols * (width + gap), body.y + slot / cols * (height + gap), width, height};
}
}  // namespace RickyPageLayout
