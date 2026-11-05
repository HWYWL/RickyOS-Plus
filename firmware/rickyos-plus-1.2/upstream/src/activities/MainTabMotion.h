#pragma once
#include <cstdint>
#include "components/Rect.h"

// Kept independent from the reader option; physical validation is required.
#define RICKYOS_MAIN_TAB_MOTION_ENABLED 1

// Render-task-owned optical history, not the currently selected (possibly
// unpainted) tab. No activity pointers, screen snapshots or heap allocations.
class MainTabMotion {
  int shown_ = -1;
  uint8_t orientation_ = 255;
  Rect content_;
  static constexpr bool valid(int index) { return index >= 0 && index < 5; }
  static constexpr bool same(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }

 public:
  void invalidate() { shown_ = -1; }
  int directionTo(int target, uint8_t orientation, Rect content, bool enabled, bool cleanup) const {
    if (!enabled || cleanup || !valid(shown_) || !valid(target) || target == shown_ ||
        orientation != orientation_ || !same(content, content_) || content.width <= 0 || content.height <= 0)
      return 0;
    return target > shown_ ? 1 : -1;
  }
  void presented(int target, uint8_t orientation, Rect content, bool success) {
    if (!success || !valid(target)) {
      invalidate();
      return;
    }
    shown_ = target;
    orientation_ = orientation;
    content_ = content;
  }
};
