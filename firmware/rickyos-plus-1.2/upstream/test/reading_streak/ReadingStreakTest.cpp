#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "util/ReadingStreak.h"

namespace {

using readingStreak::Mark;

struct Day {
  uint32_t dayOrdinal;
  uint64_t readingMs;
};

constexpr uint64_t kMinute = 60ULL * 1000ULL;
constexpr uint64_t kGoal = 30 * kMinute;
constexpr uint32_t kToday = 20000;

// One entry per minutes value, ending on `last`: minutes[i] is day last - (n - 1 - i).
std::vector<Day> history(std::initializer_list<int> minutes, const uint32_t last = kToday) {
  std::vector<Day> days;
  uint32_t day = last - static_cast<uint32_t>(minutes.size()) + 1;
  for (const int m : minutes) {
    if (m > 0) days.push_back({day, static_cast<uint64_t>(m) * kMinute});
    ++day;
  }
  return days;
}

}  // namespace

TEST(ReadingStreak, ReadingCountsWithoutMeetingTheGoal) {
  // Nine days of 12 to 25 minutes: the old goal-only rule showed 0.
  const auto days = history({12, 18, 25, 12, 20, 25, 18, 12, 23});
  const auto result = readingStreak::evaluate(days, kToday, kGoal);
  EXPECT_EQ(result.current, 9U);
  EXPECT_EQ(result.longest, 9U);
  EXPECT_TRUE(result.readToday);
}

TEST(ReadingStreak, AFewMinutesDoNotCount) {
  const auto days = history({20, 4, 20});
  EXPECT_EQ(readingStreak::evaluate(days, kToday, kGoal).current, 1U);
  EXPECT_EQ(readingStreak::markFor(5 * kMinute - 1, kGoal), Mark::None);
  EXPECT_EQ(readingStreak::markFor(5 * kMinute, kGoal), Mark::Read);
  EXPECT_EQ(readingStreak::markFor(kGoal, kGoal), Mark::Goal);
}

TEST(ReadingStreak, TodayStaysOpen) {
  // Read yesterday, nothing yet today: the streak is alive.
  const auto days = history({20, 20, 0});
  const auto result = readingStreak::evaluate(days, kToday, kGoal);
  EXPECT_EQ(result.current, 2U);
  EXPECT_FALSE(result.readToday);
}

TEST(ReadingStreak, AMissedDayWithoutACardEndsTheStreak) {
  const auto days = history({20, 20, 20, 0, 20});
  const auto result = readingStreak::evaluate(days, kToday, kGoal);
  EXPECT_EQ(result.current, 1U);
  EXPECT_EQ(result.longest, 3U);
}

TEST(ReadingStreak, SevenReadDaysEarnARestCardThatCoversAMissedDay) {
  Mark marks[10];
  const auto days = history({20, 20, 20, 20, 20, 20, 40, 0, 20, 20});
  const auto result = readingStreak::evaluate(days, kToday, kGoal, marks, kToday - 9, 10);
  EXPECT_EQ(result.current, 9U);  // the rest day keeps the chain but adds nothing
  EXPECT_EQ(result.restCards, 0);
  EXPECT_EQ(marks[6], Mark::Goal);
  EXPECT_EQ(marks[7], Mark::Rest);
  EXPECT_EQ(marks[8], Mark::Read);
}

TEST(ReadingStreak, CardsCapAtTwoAndGoWithABrokenStreak) {
  std::vector<Day> days;
  for (uint32_t d = kToday - 29; d <= kToday - 9; ++d) days.push_back({d, 20 * kMinute});  // 21 days
  auto result = readingStreak::evaluate(days, kToday - 9, kGoal);
  EXPECT_EQ(result.restCards, 2);  // three earned, two kept
  // Three missed days: two cards cover two, the third ends it.
  days.push_back({kToday - 5, 20 * kMinute});
  result = readingStreak::evaluate(days, kToday - 5, kGoal);
  EXPECT_EQ(result.current, 1U);
  EXPECT_EQ(result.restCards, 0);
  EXPECT_EQ(result.longest, 21U);
}

TEST(ReadingStreak, MarksOutsideHistoryAndInTheFutureStayEmpty) {
  Mark marks[7];
  const auto days = history({20, 40}, kToday - 2);
  readingStreak::evaluate(days, kToday, kGoal, marks, kToday - 4, 7);
  EXPECT_EQ(marks[0], Mark::None);  // before the first reading day
  EXPECT_EQ(marks[1], Mark::Read);
  EXPECT_EQ(marks[2], Mark::Goal);
  EXPECT_EQ(marks[3], Mark::None);  // missed, no card
  EXPECT_EQ(marks[5], Mark::None);  // tomorrow
}

TEST(ReadingStreak, UnsortedDaysReadTheSame) {
  auto days = history({20, 20, 0, 20, 20, 20});
  std::reverse(days.begin(), days.end());
  EXPECT_EQ(readingStreak::evaluate(days, kToday, kGoal).current, 3U);
}

TEST(ReadingStreak, EmptyHistoryAndMilestones) {
  const std::vector<Day> none;
  const auto result = readingStreak::evaluate(none, kToday, kGoal);
  EXPECT_EQ(result.current, 0U);
  EXPECT_EQ(readingStreak::nextMilestone(0), 3U);
  EXPECT_EQ(readingStreak::nextMilestone(3), 7U);
  EXPECT_EQ(readingStreak::nextMilestone(9), 14U);
  EXPECT_EQ(readingStreak::nextMilestone(1000), 0U);
}
