#include "RickyMonthReportActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/RickyBadgeArt.h"
#include "components/RickyBadgeStatus.h"
#include "components/RickyMonthSummary.h"
#include "components/RickyPageUi.h"

namespace {
namespace fui = freeink::ui;
constexpr uint64_t kMinuteMs = 60ULL * 1000ULL;

fui::Rect rect(const int x, const int y, const int w, const int h) {
  return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

// "25 分钟" or "1 小时 12 分钟".
std::string duration(const uint64_t ms) {
  const auto minutes = static_cast<unsigned>(ms / kMinuteMs);
  char text[48];
  if (minutes < 60)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_MINUTES), minutes);
  else if (minutes % 60 == 0)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_HOURS), minutes / 60);
  else
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_HOURS_MINUTES), minutes / 60, minutes % 60);
  return text;
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
}  // namespace

const char* RickyMonthReportActivity::headerTitle() const {
  snprintf(title_, sizeof(title_), tr(STR_RICKY_REPORT_TITLE), rickyMonth::monthOf(month_));
  return title_;
}

void RickyMonthReportActivity::buildScreen(UiScreen& screen) {
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
  auto right = small;
  right.align = fui::TextAlign::Right;

  const auto month = rickyMonth::summarize(month_);
  char text[160];

  // The month: total, how it was read, and against the month before.
  const int heroHeight = smallHeight + titleHeight + smallHeight * 2 + gap / 2 + pad * 2;
  const auto hero = screen.takeTop(heroHeight, gap);
  RickyPageUi::card(target, hero, false);
  int y = hero.y + pad;
  snprintf(text, sizeof(text), tr(STR_RICKY_STATS_MONTH_TITLE), rickyMonth::yearOf(month_),
           rickyMonth::monthOf(month_));
  target.text(rect(hero.x + pad, y, hero.width - pad * 2, smallHeight), text, small);
  y += smallHeight;
  target.text(rect(hero.x + pad, y, hero.width - pad * 2, titleHeight),
              month.totalMs >= kMinuteMs ? duration(month.totalMs).c_str() : tr(STR_RICKY_STATS_FACT_MONTH_EMPTY), big);
  y += titleHeight + gap / 2;
  snprintf(text, sizeof(text), tr(STR_RICKY_REPORT_SUMMARY), month.daysRead, month.finished,
           static_cast<unsigned>(month.longestStreak));
  target.text(rect(hero.x + pad, y, hero.width - pad * 2, smallHeight), text, small);
  y += smallHeight;
  if (!month.hadEarlierReading)
    snprintf(text, sizeof(text), "%s", tr(STR_RICKY_REPORT_FIRST_MONTH));
  else if (month.totalMs >= month.previousMs + kMinuteMs)
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_MORE),
             duration(month.totalMs - month.previousMs).c_str());
  else if (month.over && month.previousMs >= month.totalMs + kMinuteMs)  // a running month is only compared once ahead
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_LESS),
             duration(month.previousMs - month.totalMs).c_str());
  else
    text[0] = '\0';
  target.text(rect(hero.x + pad, y, hero.width - pad * 2, smallHeight), text, small);

  // The month's title: an ink plate with the name in paper, and why.
  const auto title = rickyMonth::titleOf(month);
  const int plateHeight = bodyHeight + gap;
  const auto named = screen.takeTop(pad * 2 + smallHeight + gap / 2 + plateHeight, gap);
  RickyPageUi::card(target, named, false);
  target.text(rect(named.x + pad, named.y + pad, named.width - pad * 2, smallHeight), tr(STR_RICKY_REPORT_NAME_LABEL),
              small);
  const char* name = I18N.get(title.name);
  auto plateText = strong;
  plateText.inverted = true;
  const int nameWidth = target.measureText(plateText.font, name, plateText).width;
  const auto plate = rect(named.x + pad, named.y + pad + smallHeight + gap / 2, nameWidth + gap * 2, plateHeight);
  renderer.fillRoundedRect(plate.x, plate.y, plate.width, plate.height, plate.height / 2, Color::Black);
  target.text(rect(plate.x + gap, plate.y + gap / 2, nameWidth, bodyHeight), name, plateText);
  snprintf(text, sizeof(text), I18N.get(title.why), title.value);
  auto why = small;
  why.maxLines = 2;
  const int whyX = plate.right() + gap;
  target.text(rect(whyX, plate.y + (plateHeight - smallHeight) / 2, named.right() - pad - whyX, smallHeight), text,
              why);

  // The books read most, each against the first.
  const int bookRow = bodyHeight + 8 + gap;
  const int booksHeight = pad * 2 + smallHeight + gap / 2 + bookRow * std::max(1, month.topCount);
  const auto books = screen.takeTop(booksHeight, gap);
  RickyPageUi::card(target, books, false);
  target.text(rect(books.x + pad, books.y + pad, books.width - pad * 2, smallHeight), tr(STR_RICKY_REPORT_TOP_BOOKS),
              small);
  y = books.y + pad + smallHeight + gap / 2;
  if (month.topCount == 0) {
    target.text(rect(books.x + pad, y, books.width - pad * 2, bodyHeight), tr(STR_RICKY_STATS_FACT_MONTH_EMPTY), body);
  }
  for (int i = 0; i < month.topCount; ++i) {
    const auto& top = month.top[i];
    const std::string time = duration(top.ms);
    const int timeWidth = 180;
    target.text(rect(books.x + pad, y, books.width - pad * 2 - timeWidth, bodyHeight), top.book->title.c_str(), body);
    target.text(rect(books.right() - pad - timeWidth, y + (bodyHeight - smallHeight) / 2, timeWidth, smallHeight),
                time.c_str(), right);
    RickyPageUi::progressBar(target, rect(books.x + pad, y + bodyHeight + 2, books.width - pad * 2, 6),
                             static_cast<int>(top.ms * 100 / month.top[0].ms));
    y += bookRow;
  }

  // The week's shape.
  const int chart = 64;
  const auto week =
      screen.takeTop(pad * 2 + smallHeight + gap / 2 + chart + 4 + smallHeight + gap / 2 + smallHeight, gap);
  RickyPageUi::card(target, week, false);
  target.text(rect(week.x + pad, week.y + pad, week.width - pad * 2, smallHeight), tr(STR_RICKY_REPORT_WEEK), small);
  const int busiest = rickyMonth::busiestWeekday(month);
  const uint64_t peak = std::max<uint64_t>(1, *std::max_element(month.byWeekday, month.byWeekday + 7));
  const int chartTop = week.y + pad + smallHeight + gap / 2;
  const int column = (week.width - pad * 2) / 7;
  const int barWidth = std::max(10, column / 2);
  for (int i = 0; i < 7; ++i) {
    const int x = week.x + pad + column * i + (column - barWidth) / 2;
    const int height = month.byWeekday[i] ? std::max(4, static_cast<int>(chart * month.byWeekday[i] / peak)) : 3;
    renderer.fillRoundedRect(x, chartTop + chart - height, barWidth, height, 3,
                             month.byWeekday[i] ? Color::Black : Color::DarkGray);
    auto label = centered;
    label.bold = i == busiest;
    target.text(rect(week.x + pad + column * i, chartTop + chart + 4, column, smallHeight),
                nthLabel(tr(STR_RICKY_STATS_WEEKDAYS), i).c_str(), label);
  }
  if (busiest >= 0) {
    const std::string day = nthLabel(tr(STR_RICKY_STATS_WEEKDAYS), busiest);
    snprintf(text, sizeof(text), tr(STR_RICKY_STATS_FACT_MONTH_WEEKDAY), day.c_str(), day.c_str());
    target.text(rect(week.x + pad, chartTop + chart + 4 + smallHeight + gap / 2, week.width - pad * 2, smallHeight),
                text, small);
  }

  // Badges earned this month, or the next one to aim for.
  const auto statuses = rickyBadgeStatus::evaluate();
  int earned[rickyBadges::kCount];
  int earnedCount = 0;
  for (size_t i = 0; i < rickyBadges::kCount; ++i)
    if (statuses[i].unlocked() && month.contains(statuses[i].unlockedDay)) earned[earnedCount++] = static_cast<int>(i);
  constexpr int kMedal = 64;
  const auto badges =
      screen.takeTop(pad * 2 + smallHeight + gap / 2 + (earnedCount ? kMedal + 4 + smallHeight : smallHeight), 0);
  RickyPageUi::card(target, badges, false);
  target.text(rect(badges.x + pad, badges.y + pad, badges.width - pad * 2, smallHeight), tr(STR_RICKY_REPORT_BADGES),
              small);
  const int rowY = badges.y + pad + smallHeight + gap / 2;
  if (earnedCount == 0) {
    const size_t next = rickyBadgeStatus::nextToEarn(statuses);
    if (next < rickyBadges::kCount) {
      snprintf(text, sizeof(text), tr(STR_RICKY_REPORT_NEXT_BADGE), rickyBadgeStatus::nameOf(next),
               static_cast<unsigned>(statuses[next].progress), static_cast<unsigned>(statuses[next].target));
      target.text(rect(badges.x + pad, rowY, badges.width - pad * 2, smallHeight), text, small);
    }
    return;
  }
  const int shown = std::min(earnedCount, 5);
  const int slot = (badges.width - pad * 2) / 5;
  for (int i = 0; i < shown; ++i) {
    const int x = badges.x + pad + slot * i;
    rickyBadgeArt::draw(renderer, rickyBadges::kDefinitions[earned[i]].icon, true, x + (slot - kMedal) / 2, rowY,
                        kMedal);
    target.text(rect(x, rowY + kMedal + 4, slot, smallHeight), rickyBadgeStatus::nameOf(earned[i]), centered);
  }
}
