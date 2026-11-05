#pragma once

#include <algorithm>
#include <cstdint>

#include "ReadingStatsStore.h"
#include "util/ReadingStreak.h"

// The RickyOS reading streak (util/ReadingStreak.h) over the store's reading days, for
// the home page and the statistics page. The store's own getCurrentStreakDays() keeps
// the goal-only count that the achievements are defined by.
namespace rickyStreak {

inline uint64_t goalMs() { return std::max<uint64_t>(60ULL * 1000ULL, getDailyReadingGoalMs()); }

inline readingStreak::Result evaluate(readingStreak::Mark* marks = nullptr, const uint32_t marksFrom = 0,
                                      const uint32_t markCount = 0) {
  const auto& stats = READING_STATS;
  return readingStreak::evaluate(stats.getReadingDays(), stats.getTodayDayOrdinal(), goalMs(), marks, marksFrom,
                                 markCount);
}

}  // namespace rickyStreak
