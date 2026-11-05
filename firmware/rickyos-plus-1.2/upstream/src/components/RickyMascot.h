#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "components/RickyCompanion.h"
#include "components/icons/rickyMascotIcons.h"

// The companion (the logo's dog, or the cheese cat with the cat theme) in three moods (rickyMascotIcons.h): ink where the bitmap is 0, paper
// left untouched, so it sits on any card.
namespace rickyMascot {

enum class Mood : uint8_t { Sleeping, Reading, Happy };

// Today against the goal: asleep before a minute of reading, reading, then happy.
constexpr Mood moodFor(const uint64_t todayMs, const uint64_t goalMs) {
  if (todayMs < 60ULL * 1000ULL) return Mood::Sleeping;
  return todayMs >= goalMs ? Mood::Happy : Mood::Reading;
}

inline const uint8_t* bitsFor(const Mood mood, const bool small) {
  if (RickyCompanion::cat()) {
    switch (mood) {
      case Mood::Sleeping:
        return small ? ricky_cat_small_sleeping : ricky_cat_sleeping;
      case Mood::Happy:
        return small ? ricky_cat_small_happy : ricky_cat_happy;
      case Mood::Reading:
        break;
    }
    return small ? ricky_cat_small_cheering : ricky_cat_cheering;
  }
  switch (mood) {
    case Mood::Sleeping:
      return small ? ricky_mascot_small_sleeping : ricky_mascot_sleeping;
    case Mood::Happy:
      return small ? ricky_mascot_small_happy : ricky_mascot_happy;
    case Mood::Reading:
      break;
  }
  return small ? ricky_mascot_small_cheering : ricky_mascot_cheering;  // cheering the reader on
}

inline void draw(const GfxRenderer& renderer, const Mood mood, const bool small, const int x, const int y) {
  const int size = small ? kRickyMascotSmallSize : kRickyMascotSize;
  const uint8_t* bits = bitsFor(mood, small);
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int column = 0; column < size; ++column) {
      if ((bits[row * rowBytes + column / 8] & (0x80U >> (column % 8))) == 0) {
        renderer.drawPixel(x + column, y + row, true);
      }
    }
  }
}

}  // namespace rickyMascot
