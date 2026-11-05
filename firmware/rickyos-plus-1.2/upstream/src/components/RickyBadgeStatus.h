#pragma once

#include <I18n.h>

#include <vector>

#include "ReadingStatsStore.h"
#include "components/RickyReadingStreak.h"
#include "util/RickyBadges.h"
#include "util/TimeUtils.h"

// RickyOS badges (util/RickyBadges.h) over the reading-stats store, with their names.
namespace rickyBadgeStatus {

inline rickyBadges::Statuses evaluate() {
  const auto& stats = READING_STATS;
  // One entry per finished book; a short-lived copy the evaluator sorts.
  std::vector<uint32_t> finished;
  finished.reserve(stats.getBooks().size());
  for (const auto& book : stats.getBooks()) {
    if (book.completed) finished.push_back(book.completedAt ? TimeUtils::getLocalDayOrdinal(book.completedAt) : 0);
  }
  return rickyBadges::evaluate(
      stats.getReadingDays(), stats.getSessionLog(), stats.getBooks(),
      [](const ReadingBookStats& book) -> const std::vector<ReadingDayStats>& { return book.readingDays; },
      std::move(finished), rickyStreak::goalMs(), stats.getTodayDayOrdinal());
}

inline constexpr StrId kNames[rickyBadges::kCount] = {
    StrId::STR_RICKY_BADGE_STREAK3,    StrId::STR_RICKY_BADGE_STREAK7,   StrId::STR_RICKY_BADGE_STREAK21,
    StrId::STR_RICKY_BADGE_STREAK50,   StrId::STR_RICKY_BADGE_STREAK100, StrId::STR_RICKY_BADGE_HOURS1,
    StrId::STR_RICKY_BADGE_HOURS10,    StrId::STR_RICKY_BADGE_HOURS50,   StrId::STR_RICKY_BADGE_HOURS100,
    StrId::STR_RICKY_BADGE_BOOKS1,     StrId::STR_RICKY_BADGE_BOOKS5,    StrId::STR_RICKY_BADGE_BOOKS10,
    StrId::STR_RICKY_BADGE_BOOKS30,    StrId::STR_RICKY_BADGE_GOAL30,    StrId::STR_RICKY_BADGE_ONE_SITTING,
    StrId::STR_RICKY_BADGE_WEEKEND,    StrId::STR_RICKY_BADGE_REST_CARD, StrId::STR_RICKY_BADGE_FULL_MONTH,
    StrId::STR_RICKY_BADGE_THREE_BOOKS};
inline constexpr StrId kHints[rickyBadges::kCount] = {
    StrId::STR_RICKY_BADGE_STREAK3_HINT,     StrId::STR_RICKY_BADGE_STREAK7_HINT,
    StrId::STR_RICKY_BADGE_STREAK21_HINT,    StrId::STR_RICKY_BADGE_STREAK50_HINT,
    StrId::STR_RICKY_BADGE_STREAK100_HINT,   StrId::STR_RICKY_BADGE_HOURS1_HINT,
    StrId::STR_RICKY_BADGE_HOURS10_HINT,     StrId::STR_RICKY_BADGE_HOURS50_HINT,
    StrId::STR_RICKY_BADGE_HOURS100_HINT,    StrId::STR_RICKY_BADGE_BOOKS1_HINT,
    StrId::STR_RICKY_BADGE_BOOKS5_HINT,      StrId::STR_RICKY_BADGE_BOOKS10_HINT,
    StrId::STR_RICKY_BADGE_BOOKS30_HINT,     StrId::STR_RICKY_BADGE_GOAL30_HINT,
    StrId::STR_RICKY_BADGE_ONE_SITTING_HINT, StrId::STR_RICKY_BADGE_WEEKEND_HINT,
    StrId::STR_RICKY_BADGE_REST_CARD_HINT,   StrId::STR_RICKY_BADGE_FULL_MONTH_HINT,
    StrId::STR_RICKY_BADGE_THREE_BOOKS_HINT};

inline const char* nameOf(const size_t index) { return I18N.get(kNames[index]); }
inline const char* hintOf(const size_t index) { return I18N.get(kHints[index]); }

inline uint32_t earnedCount(const rickyBadges::Statuses& statuses) {
  uint32_t count = 0;
  for (const auto& status : statuses) count += status.unlocked() ? 1 : 0;
  return count;
}

// The locked badge closest to being earned, by share of its target; kCount when all are earned.
inline size_t nextToEarn(const rickyBadges::Statuses& statuses) {
  size_t best = rickyBadges::kCount;
  uint64_t bestShare = 0;
  for (size_t i = 0; i < rickyBadges::kCount; ++i) {
    const auto& status = statuses[i];
    if (status.unlocked() || status.target == 0) continue;
    const uint64_t share = uint64_t(status.progress) * 1000 / status.target;
    if (best == rickyBadges::kCount || share > bestShare) {
      best = i;
      bestShare = share;
    }
  }
  return best;
}

// Bit 31 of SETTINGS.rickyBadgesSeen: the announced set has been seeded.
inline constexpr uint32_t kSeeded = 1U << 31;

}  // namespace rickyBadgeStatus
