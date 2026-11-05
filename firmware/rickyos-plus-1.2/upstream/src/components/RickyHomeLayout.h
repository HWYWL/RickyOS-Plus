#pragma once

#include <algorithm>
#include <cstdint>

// Geometry only: both orientations use the current content/safe area, not
// panel constants. The SDK components own drawing and their hit rectangles.
namespace RickyHomeLayout {
constexpr int PAGE_BOOKS = 5;  // Continue + four recent covers, as the 1.2 landing mockup.
struct Metrics {
  int columns;
  int rows;
  int rowHeight;
  int coverHeight;
};
inline Metrics fit(int width, int height, int headingHeight, int labelHeight, int gap, int padding) {
  const int columns = PAGE_BOOKS - 1;
  const int rows = 1;
  const int rowHeight = std::max(1, (height - 2 * headingHeight - (rows + 2) * gap) / (rows + 1));
  const int coverWidth = std::max(1, (width - (columns - 1) * gap) / columns - 2 * padding);
  return {columns, rows, rowHeight, std::max(1, std::min(rowHeight - labelHeight - 2 * padding, coverWidth * 3 / 2))};
}
inline int pageStart(int selection) { return std::max(0, selection) / PAGE_BOOKS * PAGE_BOOKS; }
struct BitmapFit {
  int width;
  int height;
};
// Match drawBitmap's downscale-only semantics; never crop or stretch a cover.
inline BitmapFit fitBitmap(int sourceWidth, int sourceHeight, int width, int height) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0) return {0, 0};
  if (sourceWidth <= width && sourceHeight <= height) return {sourceWidth, sourceHeight};
  if (static_cast<int64_t>(width) * sourceHeight <= static_cast<int64_t>(height) * sourceWidth)
    return {width, std::max(1, static_cast<int>(static_cast<int64_t>(sourceHeight) * width / sourceWidth))};
  return {std::max(1, static_cast<int>(static_cast<int64_t>(sourceWidth) * height / sourceHeight)), height};
}
constexpr uint8_t GREETING_COUNT = 5;
inline uint8_t chooseGreeting(uint32_t seed, uint8_t previous) {
  seed ^= seed >> 16;
  seed *= 0x7feb352dU;
  seed ^= seed >> 15;
  const uint8_t choice = seed % GREETING_COUNT;
  return choice == previous ? (choice + 1) % GREETING_COUNT : choice;
}
struct TextFit {
  int titleLines;
  bool author;
  bool progress;
};
inline TextFit fitText(int coverHeight, int titleHeight, int smallHeight, int gap, bool landscape) {
  constexpr int textProgressGap = 8;
  const bool progress = coverHeight >= titleHeight + smallHeight + textProgressGap;
  const int progressRow = progress ? smallHeight : 6;
  const int textHeight = std::max(0, coverHeight - progressRow - textProgressGap);
  const int titleLines = !landscape && textHeight >= titleHeight * 2 ? 2 : 1;
  return {titleLines, textHeight >= titleLines * titleHeight + gap + smallHeight, progress};
}
}  // namespace RickyHomeLayout
