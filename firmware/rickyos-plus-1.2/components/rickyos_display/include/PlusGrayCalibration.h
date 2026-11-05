#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace freeink::plus {
// Board diagnostic, portrait coordinates mapped exactly like GfxRenderer's
// Portrait orientation. No font/heap/framebuffer allocation or UI strings.
struct GrayCalibrationCard {
  int nativeWidth;
  int nativeHeight;
  int margin() const { return nativeHeight / 16; }
  int bandHeight() const { return nativeWidth / 6; }
  int upper() const { return nativeWidth / 5; }
  int lower() const { return nativeWidth / 2; }
  int boundary(int index) const { return margin() + index * (nativeHeight - 2 * margin()) / 16; }
  void pixel(uint8_t* frame, int x, int y, uint8_t gray) const {
    if (x < 0 || x >= nativeHeight || y < 0 || y >= nativeWidth) return;
    const int px = y, py = nativeHeight - 1 - x;
    const size_t at = static_cast<size_t>(py) * (nativeWidth / 2) + px / 2;
    const unsigned shift = (px & 1) * 4;
    frame[at] = static_cast<uint8_t>((frame[at] & ~(15u << shift)) | ((gray & 15u) << shift));
  }
  void band(uint8_t* frame, int y, bool samples) const {
    for (int index = 0; index < 16; ++index) {
      const int left = boundary(index), right = boundary(index + 1);
      for (int row = y; row < y + bandHeight(); ++row) {
        for (int x = left + 2; x < right - 1; ++x) pixel(frame, x, row, samples ? index : 15);
      }
    }
  }
  void initial(uint8_t* frame) const {
    memset(frame, 255, static_cast<size_t>(nativeWidth) * nativeHeight / 2);
    band(frame, upper(), true);
    // The lower sample interiors are WHITE throughout the full reference
    // refresh, not just assumed white by resetting the software back buffer.
    for (int y : {upper(), lower()}) {
      for (int x = boundary(0); x <= boundary(16); ++x) {
        pixel(frame, x, y - 2, 0);
        pixel(frame, x, y + bandHeight() + 1, 0);
      }
      for (int col = 0; col <= 16; ++col) {
        for (int row = y - 2; row <= y + bandHeight() + 1; ++row) pixel(frame, boundary(col), row, 0);
      }
    }
    // Rotation-independent identifiers: ONE dash = original reference;
    // TWO dashes = timed-drive samples. They are drawn in the reference pass,
    // outside the subsequently driven sample rectangle.
    for (int count = 1; count <= 2; ++count) {
      const int y = (count == 1 ? upper() : lower()) - 24;
      for (int dash = 0; dash < count; ++dash)
        for (int row = y; row < y + 5; ++row)
          for (int x = margin() + dash * 28; x < margin() + dash * 28 + 20; ++x)
            pixel(frame, x, row, 0);
    }
  }
};
}  // namespace freeink::plus
