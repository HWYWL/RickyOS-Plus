#include <gtest/gtest.h>

#include <vector>

#include "util/RickyBadges.h"

namespace {

using rickyBadges::Id;

struct Day {
  uint32_t dayOrdinal;
  uint64_t readingMs;
};
struct Session {
  uint32_t dayOrdinal;
  uint64_t sessionMs;
};
struct Book {
  std::vector<Day> readingDays;
};

constexpr uint64_t kMinute = 60ULL * 1000ULL;
constexpr uint64_t kGoal = 30 * kMinute;
// 2026-10-12 is a Monday; 2026-09-01 day ordinal below.
constexpr uint32_t kMonday = 20738;
constexpr uint32_t kSeptember1 = 20697;

rickyBadges::Statuses run(const std::vector<Day>& days, const std::vector<Session>& sessions = {},
                          const std::vector<Book>& books = {}, std::vector<uint32_t> finished = {},
                          uint32_t today = kMonday + 6) {
  return rickyBadges::evaluate(
      days, sessions, books, [](const Book& b) -> const std::vector<Day>& { return b.readingDays; },
      std::move(finished), kGoal, today);
}

const rickyBadges::Status& of(const rickyBadges::Statuses& s, Id id) { return s[static_cast<size_t>(id)]; }

std::vector<Day> daily(uint32_t from, uint32_t count, uint64_t minutes) {
  std::vector<Day> days;
  for (uint32_t i = 0; i < count; ++i) days.push_back({from + i, minutes * kMinute});
  return days;
}

}  // namespace

TEST(RickyBadges, CalendarHelpersMatchTheRealCalendar) {
  EXPECT_EQ(rickyBadges::weekdayOf(kMonday), 0);
  const auto date = rickyBadges::civil(kSeptember1);
  EXPECT_EQ(date.year, 2026);
  EXPECT_EQ(date.month, 9U);
  EXPECT_EQ(date.day, 1U);
  EXPECT_EQ(rickyBadges::civil(0).year, 1970);
}

TEST(RickyBadges, StreakBadgesFollowTheReadingStreakAndRememberTheDay) {
  const auto days = daily(kMonday - 10, 10, 10);  // ten days of ten minutes, under the goal
  const auto s = run(days, {}, {}, {}, kMonday - 1);
  EXPECT_EQ(of(s, Id::Streak3).unlockedDay, kMonday - 8);
  EXPECT_EQ(of(s, Id::Streak7).unlockedDay, kMonday - 4);
  EXPECT_FALSE(of(s, Id::Streak21).unlocked());
  EXPECT_EQ(of(s, Id::Streak21).progress, 10U);
  EXPECT_EQ(of(s, Id::Streak3).progress, 3U);  // capped at the target
  EXPECT_FALSE(of(s, Id::Goal30).unlocked());
}

TEST(RickyBadges, HoursAndGoalDaysCount) {
  const auto s = run(daily(kMonday - 40, 35, 40));  // 35 goal days, 23 h 20 min
  EXPECT_TRUE(of(s, Id::Hours1).unlocked());
  EXPECT_EQ(of(s, Id::Hours1).unlockedDay, kMonday - 39);  // the second day passes an hour
  EXPECT_TRUE(of(s, Id::Hours10).unlocked());
  EXPECT_FALSE(of(s, Id::Hours50).unlocked());
  EXPECT_EQ(of(s, Id::Hours50).progress, 23U);
  EXPECT_EQ(of(s, Id::Goal30).unlockedDay, kMonday - 11);
}

TEST(RickyBadges, FinishedBooksInOrderWithUnknownDatesLast) {
  const auto s = run({}, {}, {}, {kMonday - 3, 0, kMonday - 9, kMonday - 1, kMonday - 2});
  EXPECT_EQ(of(s, Id::Books1).unlockedDay, kMonday - 9);
  EXPECT_EQ(of(s, Id::Books5).unlockedDay, kMonday + 6);  // the fifth has no date: today
  EXPECT_EQ(of(s, Id::Books10).progress, 5U);
}

TEST(RickyBadges, OneLongSittingAndAWeekend) {
  std::vector<Day> days = {{kMonday - 2, 35 * kMinute}, {kMonday - 1, 30 * kMinute}};  // Sat, Sun
  const auto s = run(days, {{kMonday - 2, 20 * kMinute}, {kMonday - 1, 61 * kMinute}});
  EXPECT_EQ(of(s, Id::OneSitting).unlockedDay, kMonday - 1);
  EXPECT_EQ(of(s, Id::Weekend).unlockedDay, kMonday - 1);
  // Saturday short of half an hour: no weekend.
  days[0].readingMs = 29 * kMinute;
  EXPECT_FALSE(of(run(days), Id::Weekend).unlocked());
  EXPECT_EQ(of(run(days, {{kMonday, 45 * kMinute}}), Id::OneSitting).progress, 45U);
}

TEST(RickyBadges, ARestCardAndAFullMonth) {
  // September read every day except the 8th, covered by a rest card earned on the 7th.
  std::vector<Day> days = daily(kSeptember1, 30, 10);
  days.erase(days.begin() + 7);
  const auto s = run(days, {}, {}, {}, kSeptember1 + 35);
  EXPECT_EQ(of(s, Id::RestCard).unlockedDay, kSeptember1 + 7);
  EXPECT_EQ(of(s, Id::FullMonth).unlockedDay, kSeptember1 + 29);  // the 30th
  // Without the first two days the month is not full.
  days.erase(days.begin(), days.begin() + 2);
  EXPECT_FALSE(of(run(days, {}, {}, {}, kSeptember1 + 35), Id::FullMonth).unlocked());
}

TEST(RickyBadges, ThreeBooksInOneWeek) {
  std::vector<Book> books = {{{{kMonday, 20 * kMinute}}},
                             {{{kMonday + 2, 20 * kMinute}}},
                             {{{kMonday - 1, 20 * kMinute}}}};  // the third is last week's Sunday
  auto s = run({}, {}, books);
  EXPECT_FALSE(of(s, Id::ThreeBooks).unlocked());
  EXPECT_EQ(of(s, Id::ThreeBooks).progress, 2U);
  books[2].readingDays.push_back({kMonday + 4, 20 * kMinute});
  s = run({}, {}, books);
  EXPECT_EQ(of(s, Id::ThreeBooks).unlockedDay, kMonday + 4);
  EXPECT_EQ(rickyBadges::unlockedMask(s), 1U << static_cast<int>(Id::ThreeBooks));
}
