#include "RickyReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReadingDayDetailActivity.h"
#include "ReadingStatsStore.h"
#include "RickyBadgesActivity.h"
#include "RickyMonthReportActivity.h"
#include "components/RickyBadgeStatus.h"
#include "components/RickyMascot.h"
#include "components/RickyMonthSummary.h"
#include "components/RickyPageUi.h"
#include "components/RickyPaw.h"
#include "components/RickyReadingStreak.h"
#include "components/UITheme.h"
#include "util/TimeUtils.h"
#include "components/RickyCompanion.h"

namespace {
namespace fui = freeink::ui;
constexpr uint64_t kMinuteMs = 60ULL * 1000ULL;

uint32_t minutesOf(const uint64_t ms) { return static_cast<uint32_t>(ms / kMinuteMs); }

// "25 分钟" or "1 小时 12 分钟".
std::string duration(const uint64_t ms) {
  const uint32_t minutes = minutesOf(ms);
  char text[48];
  if (minutes < 60)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_MINUTES), static_cast<unsigned>(minutes));
  else if (minutes % 60 == 0)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_HOURS), static_cast<unsigned>(minutes / 60));
  else
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_HOURS_MINUTES), static_cast<unsigned>(minutes / 60),
             static_cast<unsigned>(minutes % 60));
  return text;
}

// Compact total for a small tile: whole hours once past an hour.
std::string compactDuration(const uint64_t ms) {
  const uint32_t minutes = minutesOf(ms);
  char text[32];
  if (minutes < 60)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_MINUTES), static_cast<unsigned>(minutes));
  else
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_HOURS), static_cast<unsigned>(minutes / 60));
  return text;
}

// Monday-first weekday of a day ordinal (days since 1970-01-01, a Thursday).
int weekdayOf(const uint32_t dayOrdinal) { return static_cast<int>((dayOrdinal + 3) % 7); }

uint64_t msOnDay(const std::vector<ReadingDayStats>& days, const uint32_t dayOrdinal) {
  for (const auto& day : days)
    if (day.dayOrdinal == dayOrdinal) return day.readingMs;
  return 0;
}

// The i-th code point of a short label list such as "一二三四五六日".
std::string nthLabel(const char* labels, const int index) {
  const auto* p = reinterpret_cast<const unsigned char*>(labels);
  for (int i = 0; *p; ++i) {
    const unsigned char* start = p;
    p += *p < 0x80 ? 1 : (*p < 0xE0 ? 2 : (*p < 0xF0 ? 3 : 4));
    if (i == index) return std::string(reinterpret_cast<const char*>(start), p - start);
  }
  return "";
}

// One fact per day, from what the store really holds.
std::string dailyFact(const ReadingStatsStore& stats, const uint32_t today) {
  std::vector<std::string> facts;
  char text[192];
  uint32_t longest = 0;
  for (const auto& session : stats.getSessionLog()) longest = std::max(longest, session.sessionMs);
  if (longest >= kMinuteMs) {
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_LONGEST), duration(longest).c_str());
    facts.emplace_back(text);
  }
  const ReadingDayStats* best = nullptr;
  for (const auto& day : stats.getReadingDays())
    if (!best || day.readingMs > best->readingMs) best = &day;
  int year = 0;
  unsigned month = 0, dayOfMonth = 0;
  if (best && best->readingMs >= kMinuteMs &&
      TimeUtils::getDateFromDayOrdinal(best->dayOrdinal, year, month, dayOfMonth)) {
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_BEST_DAY), month, dayOfMonth,
             duration(best->readingMs).c_str());
    facts.emplace_back(text);
  }
  const ReadingBookStats* top = nullptr;
  for (const auto& book : stats.getBooks())
    if (!top || book.totalReadingMs > top->totalReadingMs) top = &book;
  if (top && top->totalReadingMs >= kMinuteMs && !top->title.empty()) {
    std::string title = top->title;
    // Long titles keep their first dozen characters so the fact fits two lines.
    size_t cut = 0;
    int characters = 0;
    for (; cut < title.size() && characters < 12; ++characters) {
      const auto lead = static_cast<unsigned char>(title[cut]);
      cut += lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
    }
    if (cut < title.size()) title = title.substr(0, cut) + "…";
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_TOP_BOOK), title.c_str(),
             duration(top->totalReadingMs).c_str());
    facts.emplace_back(text);
  }
  if (const uint32_t finished = stats.getBooksFinishedCount()) {
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_FINISHED), static_cast<unsigned>(finished));
    facts.emplace_back(text);
  }
  if (facts.empty()) return tr(STR_RICKY_STATS_FACT_EMPTY);
  return facts[today % facts.size()];
}

// The month's own story, one fact a day; falls back to all-time facts when the month is
// too quiet to say anything.
std::string monthFact(const ReadingStatsStore& stats, const rickyMonth::Summary& month, const uint32_t today) {
  std::vector<std::string> facts;
  char text[192];
  if (month.topCount > 0 && !month.top[0].book->title.empty()) {
    std::string title = month.top[0].book->title;
    size_t cut = 0;
    int characters = 0;
    for (; cut < title.size() && characters < 10; ++characters) {
      const auto lead = static_cast<unsigned char>(title[cut]);
      cut += lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
    }
    if (cut < title.size()) title = title.substr(0, cut) + "…";
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_BOOK), title.c_str(), duration(month.top[0].ms).c_str());
    facts.emplace_back(text);
  }
  int year = 0;
  unsigned monthNumber = 0, dayOfMonth = 0;
  if (month.bestMs >= kMinuteMs && TimeUtils::getDateFromDayOrdinal(month.bestDay, year, monthNumber, dayOfMonth)) {
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_BEST), monthNumber, dayOfMonth,
             duration(month.bestMs).c_str());
    facts.emplace_back(text);
  }
  if (const int weekday = rickyMonth::busiestWeekday(month); weekday >= 0) {
    const std::string name = nthLabel(tr(STR_RICKY_STATS_WEEKDAYS), weekday);
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_WEEKDAY), name.c_str(), name.c_str());
    facts.emplace_back(text);
  }
  // A month still running is only compared once it is ahead; a finished one either way.
  if (month.previousMs >= kMinuteMs && month.totalMs >= month.previousMs + kMinuteMs) {
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_MORE),
             duration(month.totalMs - month.previousMs).c_str());
    facts.emplace_back(text);
  } else if (month.over && month.previousMs >= month.totalMs + kMinuteMs) {
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_LESS),
             duration(month.previousMs - month.totalMs).c_str());
    facts.emplace_back(text);
  }
  if (facts.empty()) return month.totalMs < kMinuteMs ? tr(STR_RICKY_STATS_FACT_MONTH_EMPTY) : dailyFact(stats, today);
  return facts[today % facts.size()];
}

// A finished month's report leads the page for the next month's first three days.
bool reportReadyDays(const uint32_t today) {
  int year = 0;
  unsigned month = 0, day = 0;
  TimeUtils::getDateFromDayOrdinal(today, year, month, day);
  return day <= 3;
}

// A chevron for the month buttons, `left` pointing back.
void drawChevron(const GfxRenderer& renderer, const int centerX, const int centerY, const bool left) {
  constexpr int kHalf = 10;
  const int tipX = left ? centerX - kHalf / 2 : centerX + kHalf / 2;
  const int tailX = left ? centerX + kHalf / 2 : centerX - kHalf / 2;
  renderer.drawLine(tailX, centerY - kHalf, tipX, centerY, 3, true);
  renderer.drawLine(tipX, centerY, tailX, centerY + kHalf, 3, true);
}

}  // namespace

const char* RickyReadingStatsActivity::headerTitle() const { return tr(STR_READING_STATS); }

void RickyReadingStatsActivity::onEnter() {
  UiListActivity::onEnter();
  const auto& stats = READING_STATS;
  currentMonth_ = rickyMonth::indexOf(stats.getTodayDayOrdinal());
  firstMonth_ = currentMonth_;
  for (const auto& day : stats.getReadingDays())
    if (day.dayOrdinal != 0) firstMonth_ = std::min(firstMonth_, rickyMonth::indexOf(day.dayOrdinal));
  viewMonth_ = currentMonth_;
  app.on(
      ACTION_PREVIOUS_MONTH,
      [](const fui::ActionEvent&, void* user) {
        auto* self = static_cast<RickyReadingStatsActivity*>(user);
        self->showMonth(self->viewMonth_ - 1);
      },
      this);
  app.on(
      ACTION_NEXT_MONTH,
      [](const fui::ActionEvent&, void* user) {
        auto* self = static_cast<RickyReadingStatsActivity*>(user);
        self->showMonth(self->viewMonth_ + 1);
      },
      this);
  app.on(
      ACTION_DAY,
      [](const fui::ActionEvent& event, void* user) {
        static_cast<RickyReadingStatsActivity*>(user)->openDay(event.value);
      },
      this);
  app.on(
      ACTION_BADGES,
      [](const fui::ActionEvent&, void* user) { static_cast<RickyReadingStatsActivity*>(user)->openBadges(); }, this);
  app.on(
      ACTION_REPORT,
      [](const fui::ActionEvent& event, void* user) {
        auto* self = static_cast<RickyReadingStatsActivity*>(user);
        // value 1: the "report is ready" line, which opens last month's.
        self->openReport(event.value == 1 ? self->viewMonth_ - 1 : self->viewMonth_);
      },
      this);
}

void RickyReadingStatsActivity::openBadges() {
  app.clearTapFlash();
  startActivityForResultWith<RickyBadgesActivity>([this](const ActivityResult&) { requestUpdate(); });
}

void RickyReadingStatsActivity::openReport(const int month) {
  app.clearTapFlash();
  startActivityForResultWith<RickyMonthReportActivity>([this](const ActivityResult&) { requestUpdate(); }, month);
}

void RickyReadingStatsActivity::showMonth(const int month) {
  const int clamped = std::clamp(month, firstMonth_, currentMonth_);
  if (clamped == viewMonth_) return;
  viewMonth_ = clamped;
  requestUpdate();
}

void RickyReadingStatsActivity::openDay(const int dayOfMonth) {
  if (dayOfMonth < 1 || dayOfMonth > static_cast<int>(rickyMonth::length(viewMonth_))) return;
  const uint32_t day = rickyMonth::firstDay(viewMonth_) + static_cast<uint32_t>(dayOfMonth - 1);
  if (day > READING_STATS.getTodayDayOrdinal()) return;
  app.clearTapFlash();
  startActivityForResultWith<ReadingDayDetailActivity>([this](const ActivityResult&) { requestUpdate(); }, day);
}

bool RickyReadingStatsActivity::handleCustomInput() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::PageBack)) {
    showMonth(viewMonth_ - 1);
    return true;
  }
  if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::PageForward)) {
    showMonth(viewMonth_ + 1);
    return true;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    openReport(viewMonth_);
    return true;
  }
  if (mappedInput.wasReleased(Button::Down)) {
    openBadges();
    return true;
  }
  return false;
}

void RickyReadingStatsActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(fui::Insets{static_cast<int16_t>(theme.spaceLg), theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);
  const int pad = gap + gap / 2;
  const int bodyHeight = target.lineHeight(theme.bodyText.font);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  const int titleHeight = target.lineHeight(theme.titleText.font);
  RickyPageUi::Bold bold(renderer, 1);
  auto small = theme.smallText;
  small.maxLines = 1;
  auto body = theme.bodyText;
  body.maxLines = 1;
  auto strong = body;
  strong.bold = true;
  auto big = theme.titleText;
  big.bold = true;
  big.maxLines = 1;
  auto centered = small;
  centered.align = fui::TextAlign::Center;
  const auto rect = [](const int x, const int y, const int w, const int h) {
    return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                     static_cast<int16_t>(h)};
  };

  const auto& stats = READING_STATS;
  const uint32_t today = stats.getTodayDayOrdinal();
  const uint64_t todayMs = stats.getTodayReadingMs();
  const uint64_t goalMs = rickyStreak::goalMs();
  const bool reached = todayMs >= goalMs;
  char text[160];

  // Today: the dog reacts to the day's reading.
  const auto mood = rickyMascot::moodFor(todayMs, goalMs);
  const int dog = kRickyMascotSmallSize;
  const int heroHeight = std::max(dog, smallHeight * 2 + titleHeight + gap * 2) + pad * 2;
  const auto hero = screen.takeTop(heroHeight, gap);
  RickyPageUi::card(target, hero, false);
  rickyMascot::draw(renderer, mood, true, hero.x + pad, hero.y + (hero.height - dog) / 2);
  const int textX = hero.x + pad + dog + gap * 2;
  const int textWidth = hero.right() - pad - textX;
  int y = hero.y + (hero.height - (smallHeight * 2 + titleHeight + gap * 2)) / 2;
  target.text(rect(textX, y, textWidth, smallHeight), tr(STR_RICKY_STATS_TODAY), small);
  y += smallHeight;
  snprintf(text, sizeof(text), tr(STR_RICKY_STATS_MINUTES), static_cast<unsigned>(minutesOf(todayMs)));
  target.text(rect(textX, y, textWidth, titleHeight), text, big);
  y += titleHeight + gap / 2;
  RickyPageUi::progressBar(target, rect(textX, y, textWidth, 8),
                           static_cast<int>(std::min<uint64_t>(100, todayMs * 100 / goalMs)));
  y += 8 + gap / 2;
  if (todayMs < kMinuteMs)
    snprintf(text, sizeof(text), "%s", RickyCompanion::text(StrId::STR_RICKY_STATS_IDLE, StrId::STR_RICKY_STATS_IDLE_CAT));
  else if (!reached)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_GOING), static_cast<unsigned>(minutesOf(goalMs - todayMs) + 1));
  else
    snprintf(text, sizeof(text), "%s", RickyCompanion::text(StrId::STR_RICKY_STATS_DONE, StrId::STR_RICKY_STATS_DONE_CAT));
  target.text(rect(textX, y, textWidth, smallHeight), text, small);

  // The month: one paw print per day read, a hatched one for a rest day.
  const uint32_t monthStart = rickyMonth::firstDay(viewMonth_);
  const unsigned monthDays = rickyMonth::length(viewMonth_);
  readingStreak::Mark marks[31];
  const auto streak = rickyStreak::evaluate(marks, monthStart, monthDays);
  const int leading = weekdayOf(monthStart);
  const int weeks = (leading + static_cast<int>(monthDays) + 6) / 7;

  const int tileHeight = titleHeight + smallHeight + pad * 2;
  const int storyHeight = smallHeight + gap / 2 + bodyHeight * 2 + pad * 2;
  const int headerHeight = 48;
  const int calendarChrome = pad * 2 + headerHeight + gap / 2 + smallHeight + gap / 2 + gap + smallHeight;
  const int rowHeight = std::clamp(
      (static_cast<int>(screen.body().height) - calendarChrome - tileHeight - storyHeight - gap * 2) / weeks, 52, 84);
  const auto calendar = screen.takeTop(calendarChrome + rowHeight * weeks, gap);
  RickyPageUi::card(target, calendar, false);

  // ‹ 2026 年 10 月 ›
  const int headerY = calendar.y + pad;
  snprintf(text, sizeof(text), tr(STR_RICKY_STATS_MONTH_TITLE), viewMonth_ / 12,
           static_cast<unsigned>(viewMonth_ % 12) + 1);
  auto title = strong;
  title.align = fui::TextAlign::Center;
  target.text(rect(calendar.x, headerY + (headerHeight - bodyHeight) / 2, calendar.width, bodyHeight), text, title);
  const int buttonWidth = 64;
  const auto previous = rect(calendar.x + pad / 2, headerY, buttonWidth, headerHeight);
  const auto next = rect(calendar.right() - pad / 2 - buttonWidth, headerY, buttonWidth, headerHeight);
  if (viewMonth_ > firstMonth_) {
    drawChevron(renderer, previous.x + buttonWidth / 2, headerY + headerHeight / 2, true);
    screen.frame().hit(previous, ACTION_PREVIOUS_MONTH, 0, fui::InputTouch);
  }
  if (viewMonth_ < currentMonth_) {
    drawChevron(renderer, next.x + buttonWidth / 2, headerY + headerHeight / 2, false);
    screen.frame().hit(next, ACTION_NEXT_MONTH, 0, fui::InputTouch);
  }

  const int gridX = calendar.x + pad;
  const int column = (calendar.width - pad * 2) / 7;
  const int labelsY = headerY + headerHeight + gap / 2;
  for (int i = 0; i < 7; ++i)
    target.text(rect(gridX + column * i, labelsY, column, smallHeight),
                nthLabel(tr(STR_RICKY_STATS_WEEKDAYS), i).c_str(), centered);
  const int gridY = labelsY + smallHeight + gap / 2;
  const int paw = std::min({36, rowHeight - smallHeight - 6, column - 12});
  for (unsigned d = 0; d < monthDays; ++d) {
    const int cellIndex = leading + static_cast<int>(d);
    const auto cell = rect(gridX + column * (cellIndex % 7), gridY + rowHeight * (cellIndex / 7), column, rowHeight);
    const uint32_t day = monthStart + d;
    auto number = centered;
    if (day > today) number.color = fui::Color::DarkGray;
    number.bold = day == today;
    snprintf(text, sizeof(text), "%u", d + 1);
    target.text(rect(cell.x, cell.y + 2, cell.width, smallHeight), text, number);
    if (day <= today && marks[d] != readingStreak::Mark::None)
      rickyPaw::draw(renderer, cell.x + (cell.width - paw) / 2, cell.y + smallHeight + 4, paw, marks[d]);
    if (day == today) renderer.drawRoundedRect(cell.x + 2, cell.y, cell.width - 4, cell.height - 4, 2, 12, true);
    if (day <= today && msOnDay(stats.getReadingDays(), day) >= kMinuteMs)
      screen.frame().hit(cell, ACTION_DAY, static_cast<int16_t>(d + 1), fui::InputTouch);
  }

  // Legend: what the three prints mean.
  const int legendY = gridY + rowHeight * weeks + gap;
  constexpr int kLegendPaw = 22;
  const readingStreak::Mark legendMarks[3] = {readingStreak::Mark::Goal, readingStreak::Mark::Read,
                                              readingStreak::Mark::Rest};
  const StrId legendNames[3] = {StrId::STR_RICKY_STATS_LEGEND_GOAL, StrId::STR_RICKY_STATS_LEGEND_READ,
                                StrId::STR_RICKY_STATS_LEGEND_REST};
  const int legendItem = (calendar.width - pad * 2) / 3;
  for (int i = 0; i < 3; ++i) {
    const int itemX = gridX + legendItem * i + (legendItem - kLegendPaw - 8 - 64) / 2;
    rickyPaw::draw(renderer, itemX, legendY + (smallHeight - kLegendPaw) / 2, kLegendPaw, legendMarks[i]);
    target.text(rect(itemX + kLegendPaw + 8, legendY, 64, smallHeight), I18N.get(legendNames[i]), small);
  }

  // The month's totals.
  const auto month = rickyMonth::summarize(viewMonth_);
  const auto tiles = screen.takeTop(tileHeight, gap);
  const int tileWidth = (tiles.width - gap * 2) / 3;
  char finished[24], days[24];
  snprintf(finished, sizeof(finished), tr(STR_RICKY_STATS_BOOKS_UNIT), month.finished);
  snprintf(days, sizeof(days), tr(STR_RICKY_STATS_DAYS_UNIT), month.daysRead);
  const std::string total = compactDuration(month.totalMs);
  const char* values[3] = {total.c_str(), days, finished};
  const StrId names[3] = {StrId::STR_RICKY_STATS_MONTH_TOTAL, StrId::STR_RICKY_STATS_DAYS,
                          StrId::STR_RICKY_STATS_FINISHED};
  auto bigCentered = big;
  bigCentered.align = fui::TextAlign::Center;
  for (int i = 0; i < 3; ++i) {
    const auto tile = rect(tiles.x + i * (tileWidth + gap), tiles.y, tileWidth, tileHeight);
    RickyPageUi::card(target, tile, false);
    target.text(rect(tile.x, tile.y + pad, tile.width, titleHeight), values[i], bigCentered);
    target.text(rect(tile.x, tile.y + pad + titleHeight, tile.width, smallHeight), I18N.get(names[i]), centered);
  }

  // The streak and the badges; one fact about the month and its report.
  const auto story = screen.takeTop(storyHeight, 0);
  RickyPageUi::card(target, story, false);
  const uint32_t streakNow = rickyStreak::evaluate().current;
  if (streak.restCards > 0)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_STREAK_REST), static_cast<unsigned>(streakNow),
             static_cast<unsigned>(streak.longest), static_cast<unsigned>(streak.restCards));
  else
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_STREAK), static_cast<unsigned>(streakNow),
             static_cast<unsigned>(streak.longest));
  constexpr int kLinkWidth = 170;
  auto link = small;
  link.align = fui::TextAlign::Right;
  link.bold = true;
  const int lineY = story.y + pad;
  target.text(rect(story.x + pad, lineY, story.width - pad * 2 - kLinkWidth, smallHeight), text, small);
  const auto statuses = rickyBadgeStatus::evaluate();
  snprintf(text, sizeof(text), tr(STR_RICKY_BADGES_LINK),
           static_cast<unsigned>(rickyBadgeStatus::earnedCount(statuses)), static_cast<unsigned>(rickyBadges::kCount));
  const auto badgesLink = rect(story.right() - pad - kLinkWidth, lineY - gap / 2, kLinkWidth, smallHeight + gap);
  target.text(rect(badgesLink.x, lineY, badgesLink.width, smallHeight), text, link);
  screen.frame().hit(badgesLink, ACTION_BADGES, 0, fui::InputTouch);

  // In a month's first days its predecessor's report leads; otherwise one fact a day.
  const int factY = lineY + smallHeight + gap / 2;
  const bool reportReady = viewMonth_ == currentMonth_ && viewMonth_ > firstMonth_ && reportReadyDays(today);
  std::string factText;
  if (reportReady) {
    snprintf(text, sizeof(text), tr(STR_RICKY_REPORT_READY), rickyMonth::monthOf(viewMonth_ - 1));
    factText = text;
  } else {
    factText = monthFact(stats, month, today);
  }
  auto fact = body;
  fact.maxLines = 2;
  fact.bold = reportReady;
  const auto factRect =
      rect(story.x + pad, factY, story.width - pad * 2 - (reportReady ? 0 : kLinkWidth / 2 + gap), bodyHeight * 2);
  target.text(factRect, factText.c_str(), fact);
  if (!reportReady)
    // Centred on the fact's two-line box, as the fact itself is.
    target.text(rect(story.right() - pad - kLinkWidth, factY + bodyHeight - smallHeight / 2, kLinkWidth, smallHeight),
                tr(STR_RICKY_REPORT_LINK), link);
  screen.frame().hit(rect(story.x, factY - gap / 2, story.width, story.bottom() - factY + gap / 2), ACTION_REPORT,
                     static_cast<int16_t>(reportReady ? 1 : 0), fui::InputTouch);
}
