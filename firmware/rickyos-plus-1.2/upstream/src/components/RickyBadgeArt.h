#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "components/RickyPaw.h"
#include "util/RickyBadges.h"

// Badge medallions, drawn from geometry so one shape serves every size. Earned: a solid
// disc with the icon cut out in paper. Not yet: a dotted ring with the icon half-toned
// (every other pixel), which reads as gray in B/W and 16-level frames alike.
namespace rickyBadgeArt {

namespace detail {
inline float square(const float v) { return v * v; }

// Inside a unit-box icon: (u, v) in [0, 1), y down.
inline bool iconAt(const rickyBadges::Icon icon, const float u, const float v) {
  using rickyBadges::Icon;
  switch (icon) {
    case Icon::Paw: {
      constexpr int kGrid = 100;
      return rickyPaw::partAt(static_cast<int>(u * kGrid), static_cast<int>(v * kGrid), kGrid, RickyCompanion::cat()) !=
             rickyPaw::Part::Outside;
    }
    case Icon::Clock: {
      const float r = std::sqrt(square(u - 0.5f) + square(v - 0.5f));
      if (r >= 0.36f && r <= 0.48f) return true;  // rim
      const bool hourHand = std::fabs(u - 0.5f) <= 0.05f && v >= 0.24f && v <= 0.54f;
      const bool minuteHand = std::fabs(v - 0.5f) <= 0.05f && u >= 0.46f && u <= 0.74f;
      return hourHand || minuteHand;
    }
    case Icon::Book: {
      // An open book: two pages sloping down to the spine.
      if (v < 0.18f || v > 0.84f || std::fabs(u - 0.5f) < 0.035f || u < 0.06f || u > 0.94f) return false;
      const float edge = std::fabs(u - 0.5f);  // 0 at the spine, 0.44 at the outer edge
      const float top = 0.30f - edge * 0.27f;
      const float bottom = 0.84f - edge * 0.27f;
      return v >= top && v <= bottom;
    }
    case Icon::Star: {
      // Five points: a ten-vertex polygon, tested by ray crossing.
      float xs[10], ys[10];
      for (int i = 0; i < 10; ++i) {
        const float angle = -3.14159265f / 2.0f + i * 3.14159265f / 5.0f;
        const float radius = i % 2 == 0 ? 0.5f : 0.21f;
        xs[i] = 0.5f + radius * std::cos(angle);
        ys[i] = 0.54f + radius * std::sin(angle);
      }
      bool inside = false;
      for (int i = 0, j = 9; i < 10; j = i++) {
        if ((ys[i] > v) != (ys[j] > v) && u < (xs[j] - xs[i]) * (v - ys[i]) / (ys[j] - ys[i]) + xs[i]) inside = !inside;
      }
      return inside;
    }
    case Icon::Hourglass: {
      if (v < 0.1f || v > 0.9f) return false;
      if (v < 0.18f || v > 0.82f) return u >= 0.16f && u <= 0.84f;  // caps
      const float half = 0.06f + std::fabs(v - 0.5f) * 0.85f;
      const bool glass = std::fabs(u - 0.5f) <= half;
      if (!glass) return false;
      const bool wall = std::fabs(u - 0.5f) >= half - 0.08f;
      const bool sand = v >= 0.62f || (v >= 0.44f && v <= 0.56f && std::fabs(u - 0.5f) <= 0.05f);
      return wall || sand;
    }
    case Icon::Calendar: {
      if (u < 0.12f || u > 0.88f || v < 0.2f || v > 0.86f) {
        // Binder rings above the page.
        return v >= 0.1f && v <= 0.28f && (std::fabs(u - 0.32f) <= 0.05f || std::fabs(u - 0.68f) <= 0.05f);
      }
      if (v <= 0.38f) return true;                              // header band
      if (u <= 0.19f || u >= 0.81f || v >= 0.79f) return true;  // frame
      const float cu = std::fmod((u - 0.19f) / 0.2067f, 1.0f), cv = std::fmod((v - 0.38f) / 0.205f, 1.0f);
      return std::fabs(cu - 0.5f) < 0.22f && std::fabs(cv - 0.5f) < 0.22f;  // day cells
    }
    case Icon::Moon: {
      const bool disc = square(u - 0.48f) + square(v - 0.5f) <= square(0.4f);
      const bool bite = square(u - 0.66f) + square(v - 0.4f) <= square(0.34f);
      return disc && !bite;
    }
    case Icon::FullMoon: {
      if (square(u - 0.5f) + square(v - 0.5f) > square(0.42f)) return false;
      const bool crater = square(u - 0.38f) + square(v - 0.38f) <= square(0.08f) ||
                          square(u - 0.62f) + square(v - 0.6f) <= square(0.11f) ||
                          square(u - 0.4f) + square(v - 0.68f) <= square(0.05f);
      return !crater;
    }
    case Icon::Stack: {
      // Three books lying on each other, each a little offset.
      constexpr float kBooks[3][3] = {{0.18f, 0.68f, 0.86f}, {0.12f, 0.46f, 0.64f}, {0.24f, 0.24f, 0.42f}};
      for (const auto& book : kBooks) {
        const float left = book[0], top = book[1], bottom = book[2];
        if (v >= top && v <= bottom && u >= left && u <= left + 0.64f) {
          const bool spine = u <= left + 0.08f;
          const bool pages = u >= left + 0.08f && v >= top + 0.05f && v <= bottom - 0.05f && u >= left + 0.56f;
          return !pages || spine;
        }
      }
      return false;
    }
  }
  return false;
}
}  // namespace detail

// Whether pixel (x, y) of a `size` medallion is ink.
inline bool inkAt(const rickyBadges::Icon icon, const bool earned, const int x, const int y, const int size) {
  const float c = (size - 1) / 2.0f;
  const float r = std::sqrt(detail::square(x - c) + detail::square(y - c));
  const float outer = size / 2.0f;
  if (r > outer) return false;
  // The icon fills the middle 56 % of the disc.
  const float box = size * 0.56f;
  const float u = (x - (size - box) / 2.0f) / box, v = (y - (size - box) / 2.0f) / box;
  const bool inIcon = u >= 0 && u < 1 && v >= 0 && v < 1 && detail::iconAt(icon, u, v);
  if (earned) {
    // Solid disc, a paper ring just inside the rim, the icon in paper.
    const bool ring = r >= outer - 7 && r <= outer - 5;
    return !(ring || inIcon);
  }
  const bool rim = r >= outer - 3;
  if (rim) return ((x + y) / 3) % 2 == 0;  // dashed rim
  return inIcon && (x + y) % 2 == 0;
}

template <typename Renderer>
void draw(const Renderer& renderer, const rickyBadges::Icon icon, const bool earned, const int originX,
          const int originY, const int size) {
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (inkAt(icon, earned, x, y, size)) renderer.drawPixel(originX + x, originY + y, true);
    }
  }
}

}  // namespace rickyBadgeArt
