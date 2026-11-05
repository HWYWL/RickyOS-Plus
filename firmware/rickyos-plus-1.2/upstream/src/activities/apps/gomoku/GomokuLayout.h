#pragma once

#include <algorithm>

// Pure geometry shared by drawing and board aiming; includes half a cell of
// edge padding. Dividing by N, not N-1, keeps the outer touch targets on-screen.
struct GomokuLayout {
  int x = 0;
  int y = 0;
  int pitch = 0;
  static GomokuLayout fit(int left, int top, int width, int height, int size) {
    if (size < 2 || width < size || height < size) return {};
    const int pitch = std::min(width, height) / size;
    return {left + (width - (size - 1) * pitch) / 2, top + (height - (size - 1) * pitch) / 2, pitch};
  }
};
