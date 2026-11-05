#pragma once

#include <algorithm>
#include <cstdint>

// RickyOS reading streak: the days in a row you read, not the days you met the goal.
//
//   Read day   at least kReadDayMs of reading. Meeting the daily goal is marked apart
//              (Mark::Goal) but is not needed to keep the streak.
//   Rest card  every kReadDaysPerRestCard read days in one streak earn a card, up to
//              kMaxRestCards. A day missed while a card is held spends it and the streak
//              carries on (Mark::Rest); a missed day without one ends the streak, and the
//              cards go with it. Rest days keep the chain but do not add to the count.
//   Today      still open: not reading yet today never breaks anything.
//
// Everything is derived from the per-day totals the store already keeps, so the rules
// need no saved state and apply to a reader's whole history at once. Pure: the host
// tests and the pages share it.
namespace readingStreak {

inline constexpr uint64_t kReadDayMs = 5ULL * 60ULL * 1000ULL;
inline constexpr uint32_t kReadDaysPerRestCard = 7;
inline constexpr uint8_t kMaxRestCards = 2;

enum class Mark : uint8_t { None, Read, Goal, Rest };

constexpr Mark markFor(const uint64_t ms, const uint64_t goalMs) {
  if (ms < kReadDayMs) return Mark::None;
  return ms >= goalMs ? Mark::Goal : Mark::Read;
}

struct Result {
  uint32_t current = 0;  // read days in the streak that is still alive today
  uint32_t longest = 0;  // the longest streak ever, by the same rules
  uint8_t restCards = 0;
  bool readToday = false;
};

// Streak lengths worth a word on the home card.
inline constexpr uint32_t kMilestones[] = {3, 7, 14, 21, 30, 50, 100, 200, 365, 500, 1000};

// The next milestone above `current`, or 0 past the last one.
constexpr uint32_t nextMilestone(const uint32_t current) {
  for (const uint32_t milestone : kMilestones) {
    if (milestone > current) return milestone;
  }
  return 0;
}

// Walks every day from the first reading day to `today` under the rules above and calls
// onDay(day, mark, streak) for each, where streak is the count after that day. `days` is
// any sequence of {dayOrdinal, readingMs} (the store's reading days), best sorted by day.
template <typename Days, typename OnDay>
Result walk(const Days& days, const uint32_t today, const uint64_t goalMs, OnDay&& onDay) {
  Result result;
  const auto begin = std::begin(days);
  const auto end = std::end(days);
  // The store keeps its days in order; anything else is still read correctly, slower.
  const bool sorted =
      std::is_sorted(begin, end, [](const auto& a, const auto& b) { return a.dayOrdinal < b.dayOrdinal; });
  uint32_t first = 0;
  for (auto day = begin; day != end; ++day) {
    if (day->dayOrdinal != 0 && (first == 0 || day->dayOrdinal < first)) first = day->dayOrdinal;
  }
  if (first == 0 || today == 0 || first > today) return result;

  auto it = begin;
  uint32_t streak = 0;
  uint32_t readInStreak = 0;
  uint8_t cards = 0;
  for (uint32_t day = first; day <= today; ++day) {
    uint64_t ms = 0;
    if (sorted) {
      while (it != end && it->dayOrdinal < day) ++it;
      if (it != end && it->dayOrdinal == day) ms = it->readingMs;
    } else {
      for (auto entry = begin; entry != end; ++entry) {
        if (entry->dayOrdinal == day) ms += entry->readingMs;
      }
    }

    Mark mark = markFor(ms, goalMs);
    if (mark != Mark::None) {
      ++streak;
      if (++readInStreak % kReadDaysPerRestCard == 0 && cards < kMaxRestCards) ++cards;
      if (day == today) result.readToday = true;
    } else if (day != today) {
      if (streak > 0 && cards > 0) {
        --cards;
        mark = Mark::Rest;
      } else {
        streak = 0;
        readInStreak = 0;
        cards = 0;
      }
    }
    result.longest = std::max(result.longest, streak);
    onDay(day, mark, streak);
  }
  result.current = streak;
  result.restCards = cards;
  return result;
}

// The streak today. `marks`, when given, receives the mark of each day in [marksFrom,
// marksFrom + markCount): what the calendar and the home card's paw prints draw. Days
// after today stay Mark::None.
template <typename Days>
Result evaluate(const Days& days, const uint32_t today, const uint64_t goalMs, Mark* marks = nullptr,
                const uint32_t marksFrom = 0, const uint32_t markCount = 0) {
  if (marks) std::fill(marks, marks + markCount, Mark::None);
  return walk(days, today, goalMs, [&](const uint32_t day, const Mark mark, uint32_t) {
    if (marks && day >= marksFrom && day - marksFrom < markCount) marks[day - marksFrom] = mark;
  });
}

}  // namespace readingStreak
