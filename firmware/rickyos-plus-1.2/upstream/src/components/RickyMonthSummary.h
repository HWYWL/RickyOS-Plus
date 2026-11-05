#pragma once

#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "ReadingStatsStore.h"
#include "components/RickyReadingStreak.h"
#include "util/TimeUtils.h"

// One calendar month of reading, for the statistics page and the month report. Months
// are indexed as year * 12 + (month - 1).
namespace rickyMonth {

inline int indexOf(const uint32_t dayOrdinal) {
  int year = 0;
  unsigned month = 0, day = 0;
  TimeUtils::getDateFromDayOrdinal(dayOrdinal, year, month, day);
  return year * 12 + static_cast<int>(month) - 1;
}
inline int yearOf(const int index) { return index / 12; }
inline unsigned monthOf(const int index) { return static_cast<unsigned>(index % 12) + 1; }
inline uint32_t firstDay(const int index) { return TimeUtils::getDayOrdinalForDate(yearOf(index), monthOf(index), 1); }
inline unsigned length(const int index) { return TimeUtils::getDaysInMonth(yearOf(index), monthOf(index)); }

struct TopBook {
  const ReadingBookStats* book = nullptr;
  uint64_t ms = 0;
};

struct Summary {
  int index = 0;
  uint32_t start = 0;
  unsigned days = 0;
  bool over = false;  // the month has ended
  uint64_t totalMs = 0;
  uint64_t previousMs = 0;
  bool hadEarlierReading = false;  // anything recorded before this month
  unsigned daysRead = 0;           // days with at least the streak's few minutes
  unsigned finished = 0;
  uint32_t bestDay = 0;
  uint64_t bestMs = 0;
  uint64_t byWeekday[7] = {};  // Monday first
  uint32_t longestStreak = 0;  // the longest streak reached within the month
  TopBook top[3];
  int topCount = 0;

  bool contains(const uint32_t day) const { return day >= start && day < start + days; }
};

inline Summary summarize(const int index) {
  const auto& stats = READING_STATS;
  const uint32_t today = stats.getTodayDayOrdinal();
  Summary s;
  s.index = index;
  s.start = firstDay(index);
  s.days = length(index);
  s.over = s.start + s.days <= today;
  const uint32_t previousStart = firstDay(index - 1);
  for (const auto& day : stats.getReadingDays()) {
    if (day.dayOrdinal != 0 && day.dayOrdinal < s.start && day.readingMs > 0) s.hadEarlierReading = true;
    if (day.dayOrdinal >= previousStart && day.dayOrdinal < s.start) s.previousMs += day.readingMs;
    if (!s.contains(day.dayOrdinal)) continue;
    s.totalMs += day.readingMs;
    s.byWeekday[(day.dayOrdinal + 3) % 7] += day.readingMs;
    if (day.readingMs >= readingStreak::kReadDayMs) ++s.daysRead;
    if (day.readingMs > s.bestMs) {
      s.bestMs = day.readingMs;
      s.bestDay = day.dayOrdinal;
    }
  }
  for (const auto& book : stats.getBooks()) {
    if (book.completed && book.completedAt != 0 && s.contains(TimeUtils::getLocalDayOrdinal(book.completedAt)))
      ++s.finished;
    uint64_t ms = 0;
    for (const auto& day : book.readingDays)
      if (s.contains(day.dayOrdinal)) ms += day.readingMs;
    if (ms < 60ULL * 1000ULL) continue;
    // Keep the three longest, longest first.
    if (s.topCount < 3) {
      s.top[s.topCount++] = {&book, ms};
    } else if (ms > s.top[2].ms) {
      s.top[2] = {&book, ms};
    } else {
      continue;
    }
    for (int i = s.topCount - 1; i > 0 && s.top[i].ms > s.top[i - 1].ms; --i) std::swap(s.top[i], s.top[i - 1]);
  }
  readingStreak::walk(stats.getReadingDays(), today, rickyStreak::goalMs(),
                      [&](const uint32_t day, readingStreak::Mark, const uint32_t streak) {
                        if (s.contains(day)) s.longestStreak = std::max(s.longestStreak, streak);
                      });
  return s;
}

// The month's name for the reader, and the line that explains it.
struct Title {
  StrId name;
  StrId why;
  unsigned value = 0;  // the number the explanation quotes, if any
};

inline Title titleOf(const Summary& s) {
  constexpr uint64_t kHour = 60ULL * 60ULL * 1000ULL;
  const uint64_t weekend = s.byWeekday[5] + s.byWeekday[6];
  if (s.over && s.daysRead == s.days)
    return {StrId::STR_RICKY_REPORT_NAME_ALL_DAYS, StrId::STR_RICKY_REPORT_WHY_ALL_DAYS};
  if (s.finished >= 3) return {StrId::STR_RICKY_REPORT_NAME_FINISHER, StrId::STR_RICKY_REPORT_WHY_FINISHER, s.finished};
  if (s.daysRead >= 4 && weekend * 2 > s.totalMs)
    return {StrId::STR_RICKY_REPORT_NAME_WEEKEND, StrId::STR_RICKY_REPORT_WHY_WEEKEND};
  if (s.totalMs >= 2 * kHour && s.bestMs * 10 >= s.totalMs * 4)
    return {StrId::STR_RICKY_REPORT_NAME_SPRINT, StrId::STR_RICKY_REPORT_WHY_SPRINT,
            static_cast<unsigned>(s.bestMs * 100 / s.totalMs)};
  if (s.totalMs >= 30 * kHour)
    return {StrId::STR_RICKY_REPORT_NAME_DEEP, StrId::STR_RICKY_REPORT_WHY_DEEP,
            static_cast<unsigned>(s.totalMs / kHour)};
  if (s.daysRead >= 15) return {StrId::STR_RICKY_REPORT_NAME_STEADY, StrId::STR_RICKY_REPORT_WHY_STEADY, s.daysRead};
  return {StrId::STR_RICKY_REPORT_NAME_SLOW, StrId::STR_RICKY_REPORT_WHY_SLOW};
}

// The weekday that carries the month, Monday = 0; -1 with fewer than three days read.
inline int busiestWeekday(const Summary& s) {
  if (s.daysRead < 3) return -1;
  return static_cast<int>(std::max_element(s.byWeekday, s.byWeekday + 7) - s.byWeekday);
}

}  // namespace rickyMonth
