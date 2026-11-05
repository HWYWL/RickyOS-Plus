#include "RickyBadgesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "components/RickyBadgeArt.h"
#include "components/RickyBadgeStatus.h"
#include "components/RickyPageUi.h"
#include "util/TimeUtils.h"

namespace {
namespace fui = freeink::ui;

fui::Rect rect(const int x, const int y, const int w, const int h) {
  return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)};
}
}  // namespace

const char* RickyBadgesActivity::headerTitle() const { return tr(STR_RICKY_BADGES_TITLE); }

void RickyBadgesActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(fui::Insets{static_cast<int16_t>(theme.spaceLg), theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  RickyPageUi::Bold bold(renderer, 1);
  auto small = theme.smallText;
  small.maxLines = 1;
  auto centered = small;
  centered.align = fui::TextAlign::Center;
  auto name = centered;
  name.bold = true;
  auto quiet = centered;
  quiet.color = fui::Color::DarkGray;

  const auto statuses = rickyBadgeStatus::evaluate();
  char text[96];

  // 已得 7/19 · next up.
  const auto summary = screen.takeTop(smallHeight + gap, gap);
  snprintf(text, sizeof(text), tr(STR_RICKY_BADGES_COUNT),
           static_cast<unsigned>(rickyBadgeStatus::earnedCount(statuses)), static_cast<unsigned>(rickyBadges::kCount));
  target.text(rect(summary.x, summary.y, summary.width / 2, smallHeight), text, small);
  const size_t next = rickyBadgeStatus::nextToEarn(statuses);
  if (next < rickyBadges::kCount) {
    snprintf(text, sizeof(text), tr(STR_RICKY_BADGES_NEXT), rickyBadgeStatus::nameOf(next),
             static_cast<unsigned>(statuses[next].progress), static_cast<unsigned>(statuses[next].target));
    auto right = small;
    right.align = fui::TextAlign::Right;
    target.text(rect(summary.x + summary.width / 3, summary.y, summary.width * 2 / 3, smallHeight), text, right);
  }

  // Four across; each badge a medallion over its name and one line about it.
  constexpr int kColumns = 4;
  const auto grid = screen.body();
  const int rows = (static_cast<int>(rickyBadges::kCount) + kColumns - 1) / kColumns;
  const int column = grid.width / kColumns;
  const int rowHeight = std::min<int>(grid.height / rows, 200);
  const int medal = std::min({88, column - 24, rowHeight - smallHeight * 3 - gap});
  for (size_t i = 0; i < rickyBadges::kCount; ++i) {
    const auto& status = statuses[i];
    const int x = grid.x + column * static_cast<int>(i % kColumns);
    const int y = grid.y + rowHeight * static_cast<int>(i / kColumns);
    rickyBadgeArt::draw(renderer, rickyBadges::kDefinitions[i].icon, status.unlocked(), x + (column - medal) / 2, y,
                        medal);
    const int textY = y + medal + 6;
    target.text(rect(x, textY, column, smallHeight), rickyBadgeStatus::nameOf(i), name);
    if (status.unlocked()) {
      int year = 0;
      unsigned month = 0, day = 0;
      TimeUtils::getDateFromDayOrdinal(status.unlockedDay, year, month, day);
      snprintf(text, sizeof(text), tr(STR_RICKY_BADGES_EARNED_ON), month, day);
      target.text(rect(x, textY + smallHeight, column, smallHeight), text, quiet);
      continue;
    }
    target.text(rect(x, textY + smallHeight, column, smallHeight), rickyBadgeStatus::hintOf(i), quiet);
    if (status.target > 1 && status.progress > 0) {
      snprintf(text, sizeof(text), tr(STR_RICKY_BADGES_PROGRESS), static_cast<unsigned>(status.progress),
               static_cast<unsigned>(status.target));
      target.text(rect(x, textY + smallHeight * 2, column, smallHeight), text, quiet);
    }
  }
}
