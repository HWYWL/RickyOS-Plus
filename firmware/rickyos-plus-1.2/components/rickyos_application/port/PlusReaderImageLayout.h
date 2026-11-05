#pragma once

#include <cstdint>

namespace plusReaderImage {

// Keep a small safe edge beyond the board's oriented insets. Use the smaller
// available margin, so asymmetric insets never put an image outside the panel.
inline int horizontalOutset(const int textLeft, const int textRight, const int viewableLeft,
                            const int viewableRight) {
  constexpr int kImageEdge = 8;
  const int left = textLeft - viewableLeft - kImageEdge;
  const int right = textRight - viewableRight - kImageEdge;
  const int available = left < right ? left : right;
  return available > 0 ? available : 0;
}

// Chapter art may use the whole image viewport, independently of its publisher's
// percentage width. Keep small ornaments / inline glyphs at their CSS size. This
// is layout-time sizing: pagination must reserve the enlarged height as well.
// No buffers or heap allocations; all products fit int64_t even for corrupt input.
inline bool fitChapterArt(const int sourceWidth, const int sourceHeight, const int viewportWidth,
                          const int viewportHeight, const int imageOutset, int& width, int& height) {
  if (sourceWidth < 240 || sourceHeight < 80 || viewportWidth <= 0 || viewportHeight <= 0 ||
      width <= 0 || height <= 0 || int64_t(width) * 2 < viewportWidth ||
      int64_t(height) * 12 < viewportWidth || imageOutset < 0) {
    return false;
  }

  // Fit the original aspect, not a potentially distorted CSS width+height pair.
  // A tall illustration is height-limited instead of cropped to full width.
  const int64_t imageWidth = int64_t(viewportWidth) + 2LL * imageOutset;
  if (imageWidth > INT32_MAX) return false;
  if (int64_t(sourceHeight) * imageWidth <= int64_t(sourceWidth) * viewportHeight) {
    width = static_cast<int>(imageWidth);
    height = static_cast<int>((int64_t(sourceHeight) * width + sourceWidth / 2) / sourceWidth);
  } else {
    height = viewportHeight;
    width = static_cast<int>((int64_t(sourceWidth) * height + sourceHeight / 2) / sourceHeight);
  }
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  return true;
}

}  // namespace plusReaderImage
