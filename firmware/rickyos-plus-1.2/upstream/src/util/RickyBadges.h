#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "util/ReadingStreak.h"

// RickyOS badges: small goals around the reading streak, time, finished books and a few
// playful habits. Every badge is derived from the reading history the store already
// keeps (days, sessions, books), with the day it was first earned, so nothing but the
// set already announced needs saving. Pure: the host tests and the pages share it.
namespace rickyBadges {

enum class Id : uint8_t {
  Streak3,
  Streak7,
  Streak21,
  Streak50,
  Streak100,
  Hours1,
  Hours10,
  Hours50,
  Hours100,
  Books1,
  Books5,
  Books10,
  Books30,
  Goal30,
  OneSitting,
  Weekend,
  RestCard,
  FullMonth,
  ThreeBooks,
  Count
};
inline constexpr size_t kCount = static_cast<size_t>(Id::Count);

enum class Icon : uint8_t { Paw, Clock, Book, Star, Hourglass, Calendar, Moon, FullMoon, Stack };

struct Definition {
  Id id;
  Icon icon;
  uint32_t target;  // in the badge's own unit: days, hours, books, minutes, or 1 for a yes/no badge
};

inline constexpr Definition kDefinitions[kCount] = {
    {Id::Streak3, Icon::Paw, 3},      {Id::Streak7, Icon::Paw, 7},     {Id::Streak21, Icon::Paw, 21},
    {Id::Streak50, Icon::Paw, 50},    {Id::Streak100, Icon::Paw, 100}, {Id::Hours1, Icon::Clock, 1},
    {Id::Hours10, Icon::Clock, 10},   {Id::Hours50, Icon::Clock, 50},  {Id::Hours100, Icon::Clock, 100},
    {Id::Books1, Icon::Book, 1},      {Id::Books5, Icon::Book, 5},     {Id::Books10, Icon::Book, 10},
    {Id::Books30, Icon::Book, 30},    {Id::Goal30, Icon::Star, 30},    {Id::OneSitting, Icon::Hourglass, 60},
    {Id::Weekend, Icon::Calendar, 1}, {Id::RestCard, Icon::Moon, 1},   {Id::FullMonth, Icon::FullMoon, 1},
    {Id::ThreeBooks, Icon::Stack, 3},
};

inline constexpr uint64_t kMinuteMs = 60ULL * 1000ULL;
inline constexpr uint64_t kHourMs = 60ULL * kMinuteMs;
inline constexpr uint64_t kWeekendDayMs = 30ULL * kMinuteMs;  // each of Saturday and Sunday

struct Status {
  uint32_t progress = 0;  // toward target, capped at it
  uint32_t target = 0;
  uint32_t unlockedDay = 0;  // day ordinal first earned; 0 = still locked
  bool unlocked() const { return unlockedDay != 0; }
};

using Statuses = std::array<Status, kCount>;

// Monday-first weekday of a day ordinal (1970-01-01 was a Thursday).
constexpr int weekdayOf(const uint32_t day) { return static_cast<int>((day + 3) % 7); }

struct DayOf {
  int year;
  unsigned month;
  unsigned day;
};
// Civil date of a day ordinal (Howard Hinnant's algorithm).
constexpr DayOf civil(const uint32_t ordinal) {
  const int64_t z = static_cast<int64_t>(ordinal) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0)), m, d};
}

// days      the store's reading days, {dayOrdinal, readingMs}, sorted by day
// sessions  {dayOrdinal, sessionMs} per reading session, any order
// books     any sequence; bookDays(book) gives its {dayOrdinal, readingMs} days
// finishDays the day ordinal each finished book was finished (0 when unknown)
template <typename Days, typename Sessions, typename Books, typename BookDays>
Statuses evaluate(const Days& days, const Sessions& sessions, const Books& books, BookDays&& bookDays,
                  std::vector<uint32_t> finishDays, const uint64_t goalMs, const uint32_t today) {
  Statuses statuses{};
  for (size_t i = 0; i < kCount; ++i) statuses[i].target = kDefinitions[i].target;
  auto& at = statuses;
  const auto earn = [&](const Id id, const uint32_t day) {
    auto& status = at[static_cast<size_t>(id)];
    if (status.unlockedDay == 0 && day != 0) status.unlockedDay = day;
  };
  const auto raise = [&](const Id id, const uint64_t value) {
    auto& status = at[static_cast<size_t>(id)];
    status.progress =
        static_cast<uint32_t>(std::max<uint64_t>(status.progress, std::min<uint64_t>(value, status.target)));
  };

  // Streaks, rest cards and full months, day by day under the streak rules.
  constexpr Id kStreaks[] = {Id::Streak3, Id::Streak7, Id::Streak21, Id::Streak50, Id::Streak100};
  uint32_t monthMarked = 0;
  int monthKey = -1;
  readingStreak::walk(days, today, goalMs,
                      [&](const uint32_t day, const readingStreak::Mark mark, const uint32_t streak) {
                        for (const Id id : kStreaks) {
                          raise(id, streak);
                          if (streak >= kDefinitions[static_cast<size_t>(id)].target) earn(id, day);
                        }
                        if (mark == readingStreak::Mark::Rest) {
                          raise(Id::RestCard, 1);
                          earn(Id::RestCard, day);
                        }
                        const DayOf date = civil(day);
                        const int key = date.year * 12 + static_cast<int>(date.month);
                        if (key != monthKey) {
                          monthKey = key;
                          monthMarked = 0;
                        }
                        // A month whose every day, from the 1st, carries a print.
                        if (mark != readingStreak::Mark::None && monthMarked + 1 == date.day) ++monthMarked;
                        const DayOf tomorrow = civil(day + 1);
                        if (tomorrow.month != date.month && monthMarked == date.day) {
                          raise(Id::FullMonth, 1);
                          earn(Id::FullMonth, day);
                        }
                      });

  // Time and the daily goal.
  uint64_t totalMs = 0;
  uint32_t goalDays = 0;
  constexpr Id kHours[] = {Id::Hours1, Id::Hours10, Id::Hours50, Id::Hours100};
  for (const auto& day : days) {
    if (day.dayOrdinal == 0 || day.dayOrdinal > today) continue;
    totalMs += day.readingMs;
    for (const Id id : kHours) {
      if (totalMs >= kDefinitions[static_cast<size_t>(id)].target * kHourMs) earn(id, day.dayOrdinal);
    }
    if (day.readingMs >= goalMs && ++goalDays == kDefinitions[static_cast<size_t>(Id::Goal30)].target)
      earn(Id::Goal30, day.dayOrdinal);
  }
  for (const Id id : kHours) raise(id, totalMs / kHourMs);
  raise(Id::Goal30, goalDays);

  // Finished books, in the order they were finished (unknown dates count, dated last).
  std::sort(finishDays.begin(), finishDays.end(),
            [](const uint32_t a, const uint32_t b) { return (a == 0 ? UINT32_MAX : a) < (b == 0 ? UINT32_MAX : b); });
  constexpr Id kBooks[] = {Id::Books1, Id::Books5, Id::Books10, Id::Books30};
  for (const Id id : kBooks) {
    const uint32_t target = kDefinitions[static_cast<size_t>(id)].target;
    raise(id, finishDays.size());
    if (finishDays.size() >= target) earn(id, finishDays[target - 1] != 0 ? finishDays[target - 1] : today);
  }

  // One long sitting.
  uint64_t longest = 0;
  uint32_t sittingDay = 0;
  const uint64_t sittingMs = kDefinitions[static_cast<size_t>(Id::OneSitting)].target * kMinuteMs;
  for (const auto& session : sessions) {
    longest = std::max<uint64_t>(longest, session.sessionMs);
    if (session.sessionMs >= sittingMs && session.dayOrdinal != 0 &&
        (sittingDay == 0 || session.dayOrdinal < sittingDay))
      sittingDay = session.dayOrdinal;
  }
  raise(Id::OneSitting, longest / kMinuteMs);
  earn(Id::OneSitting, sittingDay);

  // A weekend read on both days.
  uint64_t saturdayMs = 0;
  uint32_t saturday = 0;
  for (const auto& day : days) {
    if (day.dayOrdinal == 0 || day.dayOrdinal > today) continue;
    if (weekdayOf(day.dayOrdinal) == 5) {
      saturday = day.dayOrdinal;
      saturdayMs = day.readingMs;
    } else if (weekdayOf(day.dayOrdinal) == 6 && saturday + 1 == day.dayOrdinal && saturdayMs >= kWeekendDayMs &&
               day.readingMs >= kWeekendDayMs) {
      raise(Id::Weekend, 1);
      earn(Id::Weekend, day.dayOrdinal);
    }
  }

  // Three different books within one Monday-to-Sunday week: earned the day the third
  // one was opened.
  struct Opened {
    uint32_t week;
    uint32_t day;
  };
  std::vector<Opened> opened;
  for (const auto& book : books) {
    // The first day this book was read in each week.
    uint32_t lastWeek = UINT32_MAX;
    for (const auto& day : bookDays(book)) {
      if (day.dayOrdinal == 0 || day.dayOrdinal > today || day.readingMs < kMinuteMs) continue;
      const uint32_t week = (day.dayOrdinal + 3) / 7;
      if (week == lastWeek) continue;
      lastWeek = week;
      opened.push_back({week, day.dayOrdinal});
    }
  }
  std::sort(opened.begin(), opened.end(),
            [](const Opened& a, const Opened& b) { return a.week != b.week ? a.week < b.week : a.day < b.day; });
  const uint32_t needed = kDefinitions[static_cast<size_t>(Id::ThreeBooks)].target;
  for (size_t i = 0; i < opened.size();) {
    size_t j = i;
    while (j < opened.size() && opened[j].week == opened[i].week) ++j;
    raise(Id::ThreeBooks, j - i);
    if (j - i >= needed) {
      const uint32_t day = opened[i + needed - 1].day;
      auto& status = at[static_cast<size_t>(Id::ThreeBooks)];
      if (status.unlockedDay == 0 || day < status.unlockedDay) status.unlockedDay = day;
    }
    i = j;
  }
  return statuses;
}

// Bit i set for each unlocked badge.
inline uint32_t unlockedMask(const Statuses& statuses) {
  uint32_t mask = 0;
  for (size_t i = 0; i < kCount; ++i) {
    if (statuses[i].unlocked()) mask |= 1U << i;
  }
  return mask;
}

static_assert(kCount < 31, "the announced set is a 32-bit mask with a seeded bit");

}  // namespace rickyBadges
