#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Allocation-free physical-frame policy. Only successful panel transactions
// advance this ledger; a failed/foreign frame resets it. No optical assumptions
// about a short DU waveform: even small feedback retains all 16 gray levels.
namespace ricky::gray {
struct Area {
  int x = 0, y = 0, width = 0, height = 0;
  bool empty() const { return width <= 0 || height <= 0; }
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

inline Area unite(Area a, Area b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
  return {x, y, std::max(a.x + a.width, b.x + b.width) - x, std::max(a.y + a.height, b.y + b.height) - y};
}

// epdiy's difference kernel expands columns to 32 px. Put the entire aligned
// footer strip in its own zone, including the boundary's few content pixels,
// so a content cleanup cannot drive the footer by alignment spillover.
inline Area navigationZone(Area bar, int width, int height) {
  if (bar.empty()) return {};
  int x0 = std::clamp(bar.x, 0, width) & ~31;
  int x1 = std::min(width, (std::clamp(bar.x + bar.width, 0, width) + 31) & ~31);
  int y0 = std::clamp(bar.y, 0, height);
  int y1 = std::clamp(bar.y + bar.height, 0, height);
  // Page layouts can inset the bar for the bezel/button hints. Extend that
  // known navigation bar across the edge strip, not an interior control.
  if (bar.height >= height / 2 && bar.width <= width / 3) {
    if (x0 <= width / 4) return {0, 0, x1, height};
    if (x1 >= width * 3 / 4) return {x0, 0, width - x0, height};
  }
  if (bar.width >= width / 2 && bar.height <= height / 3) {
    if (y0 <= height / 4) return {0, 0, width, y1};
    if (y1 >= height * 3 / 4) return {0, y0, width, height - y0};
  }
  return {};
}

struct Damage {
  Area content, navigation;
  // Gray travel is the sum of abs(oldGray-newGray), not bounding-box area.
  // Measured in the existing difference walk; no extra framebuffer or pass.
  uint64_t contentTravel = 0, navigationTravel = 0;
  uint32_t contentPixels = 0, navigationPixels = 0;
  uint32_t contentCapacity = 0, navigationCapacity = 0;
};

inline Damage difference(const uint8_t* front, const uint8_t* back, int width, int height, Area nav) {
  Damage result;
  if (!front || !back || width <= 0 || (width & 1) || height <= 0) return result;
  const int nx0 = std::clamp(nav.x, 0, width), ny0 = std::clamp(nav.y, 0, height);
  const int nx1 = std::clamp(nav.x + std::max(0, nav.width), 0, width);
  const int ny1 = std::clamp(nav.y + std::max(0, nav.height), 0, height);
  result.navigationCapacity = static_cast<uint32_t>(nx1 - nx0) * (ny1 - ny0);
  result.contentCapacity = static_cast<uint32_t>(width) * height - result.navigationCapacity;
  const int stride = width / 2;
  for (int y = 0; y < height; ++y) {
    int contentLeft = width, contentRight = -1, navLeft = width, navRight = -1;
    for (int bx = 0; bx < stride; ++bx) {
      const size_t offset = static_cast<size_t>(y) * stride + bx;
      const uint8_t changed = front[offset] ^ back[offset];
      if (!changed) continue;
      for (int half = 0; half < 2; ++half) {
        if (!(changed & (half == 0 ? 0x0F : 0xF0))) continue;
        const int x = bx * 2 + half;
        const bool inNav = nav.contains(x, y);
        const int shift = half * 4;
        const int oldGray = (back[offset] >> shift) & 15;
        const int newGray = (front[offset] >> shift) & 15;
        const unsigned travel = oldGray > newGray ? oldGray - newGray : newGray - oldGray;
        (inNav ? result.navigationTravel : result.contentTravel) += travel;
        ++(inNav ? result.navigationPixels : result.contentPixels);
        int& left = inNav ? navLeft : contentLeft;
        int& right = inNav ? navRight : contentRight;
        left = std::min(left, x);
        right = x;  // Ascending scan, no per-pixel rectangle union.
      }
    }
    if (contentRight >= 0) result.content = unite(result.content, {contentLeft, y, contentRight - contentLeft + 1, 1});
    if (navRight >= 0) result.navigation = unite(result.navigation, {navLeft, y, navRight - navLeft + 1, 1});
  }
  return result;
}

struct Step {
  Area area;
  bool cleanup = false;
};
struct Plan {
  Step steps[2]{};
  uint8_t count = 0;
};

struct Ledger {
  Area nav, contentDebt, navigationDebt;
  uint8_t contentFrames = 0, smallFrames = 0, navigationFrames = 0;
  uint64_t contentTravel = 0, navigationTravel = 0;

  Plan advance(const Damage& damage, Area nextNav, uint8_t contentEvery, uint8_t smallEvery, bool adaptive = false) {
    if (nav.x != nextNav.x || nav.y != nextNav.y || nav.width != nextNav.width || nav.height != nextNav.height) {
      *this = {};
      nav = nextNav;
    }
    Plan plan;
    const bool small = !damage.content.empty() && damage.content.width <= 64 && damage.content.height <= 64;
    bool contentClean = false, navClean = false;
    if (!damage.content.empty()) {
      contentDebt = unite(contentDebt, damage.content);
      if (small)
        smallFrames = std::min(255, smallFrames + 1);
      else
        contentFrames = std::min(255, contentFrames + 1);
      contentTravel += damage.contentTravel;
      // Experimental pressure estimate, not optical ghost detection. Bound
      // sparse repeated changes too; never promise indefinitely clean GL16.
      if (adaptive && damage.contentCapacity) {
        contentClean = contentTravel >= uint64_t{damage.contentCapacity} * 15 * 6 ||
                       unsigned(contentFrames) + smallFrames >= 64;
      } else {
        contentClean = (contentEvery && contentFrames >= contentEvery) || (smallEvery && smallFrames >= smallEvery);
      }
    }
    if (!damage.navigation.empty()) {
      navigationDebt = unite(navigationDebt, damage.navigation);
      navigationFrames = std::min(255, navigationFrames + 1);
      navigationTravel += damage.navigationTravel;
      navClean = adaptive && damage.navigationCapacity
                     ? navigationTravel >= uint64_t{damage.navigationCapacity} * 15 * 12 || navigationFrames >= 128
                     : smallEvery && navigationFrames >= smallEvery;
    }
    if (!contentClean && !navClean) {
      const Area area = unite(damage.content, damage.navigation);
      if (!area.empty()) plan.steps[plan.count++] = {area, false};
      return plan;
    }
    // A cleanup covers all accumulated debt, not just the latest change.
    // At most two submissions; never add a second pass to ordinary feedback.
    if (!damage.content.empty()) plan.steps[plan.count++] = {contentClean ? contentDebt : damage.content, contentClean};
    if (!damage.navigation.empty())
      plan.steps[plan.count++] = {navClean ? navigationDebt : damage.navigation, navClean};
    if (contentClean) {
      contentDebt = {};
      contentFrames = smallFrames = 0;
      contentTravel = 0;
    }
    if (navClean) {
      navigationDebt = {};
      navigationFrames = 0;
      navigationTravel = 0;
    }
    return plan;
  }
};
}  // namespace ricky::gray
