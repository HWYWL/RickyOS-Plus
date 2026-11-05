#pragma once

#include <cstdint>

#include "components/RickyCompanion.h"
#include "util/ReadingStreak.h"

// The companion's paw print (dog, or with the cat theme a cat's): the stamp the reading streak and the month calendar draw for a day.
// Drawn from geometry (a main pad and four toes), so one shape serves every size, in plain
// ink that reads the same in B/W and 16-level frames:
//
//   Goal   solid           the daily goal met
//   Read   outlined        read, under the goal
//   Rest   hatched         a rest card kept the streak
//   None   dotted outline  a print still to make: today before reading, or a missed day
namespace rickyPaw {

enum class Part : uint8_t { Outside, Pad, Toe };

// A cat's paw: one round pad and four small oval toes set close above it in a tight arc.
constexpr Part catPartAt(const int px, const int py, const int s) {
  const auto inEllipse = [](long long dx, long long dy, long long rx, long long ry) {
    return dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry;
  };
  if (inEllipse(px - 50 * s, py - 66 * s, 25 * s, 20 * s)) return Part::Pad;
  constexpr int kToes[4][2] = {{22, 38}, {40, 24}, {60, 24}, {78, 38}};
  for (const auto& toe : kToes) {
    if (inEllipse(px - toe[0] * s, py - toe[1] * s, 9 * s, 11 * s)) return Part::Toe;
  }
  return Part::Outside;
}

// Which part of a `size` x `size` paw the pixel (x, y) falls in. Fractions of the size,
// scaled by 100 to stay in integers.
constexpr Part partAt(const int x, const int y, const int size, const bool cat = false) {
  if (size <= 0 || x < 0 || y < 0 || x >= size || y >= size) return Part::Outside;
  const int px = x * 100 + 50;  // pixel centre, in hundredths of a pixel
  const int py = y * 100 + 50;
  const int s = size;
  if (cat) return catPartAt(px, py, s);
  // Main pad: an ellipse low in the box.
  const int cx = 50 * s, cy = 68 * s, rx = 30 * s, ry = 24 * s;
  const long long dx = px - cx, dy = py - cy;
  if (dx * dx * ry * ry + dy * dy * rx * rx <= 1LL * rx * rx * ry * ry) return Part::Pad;
  // Toes: four discs fanned over the pad.
  constexpr int kToes[4][2] = {{17, 40}, {35, 17}, {65, 17}, {83, 40}};
  const int r = 12 * s;
  for (const auto& toe : kToes) {
    const long long tx = px - toe[0] * s, ty = py - toe[1] * s;
    if (tx * tx + ty * ty <= 1LL * r * r) return Part::Toe;
  }
  return Part::Outside;
}

// On the outline: inside the shape with an outside pixel within `stroke` pixels.
constexpr bool onEdge(const int x, const int y, const int size, const int stroke, const bool cat = false) {
  if (partAt(x, y, size, cat) == Part::Outside) return false;
  for (int d = 1; d <= stroke; ++d) {
    if (partAt(x - d, y, size, cat) == Part::Outside || partAt(x + d, y, size, cat) == Part::Outside ||
        partAt(x, y - d, size, cat) == Part::Outside || partAt(x, y + d, size, cat) == Part::Outside) {
      return true;
    }
  }
  return false;
}

// Whether the pixel is ink for a paw in `mark`'s look.
constexpr bool inkAt(const int x, const int y, const int size, const readingStreak::Mark mark,
                     const bool cat = false) {
  const Part part = partAt(x, y, size, cat);
  if (part == Part::Outside) return false;
  const int stroke = size >= 40 ? 3 : 2;
  switch (mark) {
    case readingStreak::Mark::Goal:
      return true;
    case readingStreak::Mark::Read:
      return onEdge(x, y, size, stroke, cat);
    case readingStreak::Mark::Rest:
      return onEdge(x, y, size, stroke, cat) || (x + y) % 4 == 0;
    case readingStreak::Mark::None:
      break;
  }
  return onEdge(x, y, size, 1, cat) && (x + y) % 2 == 0;
}

template <typename Renderer>
void draw(const Renderer& renderer, const int originX, const int originY, const int size,
          const readingStreak::Mark mark, const bool cat = RickyCompanion::cat()) {
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (inkAt(x, y, size, mark, cat)) renderer.drawPixel(originX + x, originY + y, true);
    }
  }
}

}  // namespace rickyPaw
