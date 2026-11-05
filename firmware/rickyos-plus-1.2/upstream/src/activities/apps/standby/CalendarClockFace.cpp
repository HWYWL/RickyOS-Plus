#include "CalendarClockFace.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/RickyClockDigits.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMargin = 56;
constexpr int kTitleFont = NOTOSANS_18_FONT_ID;
constexpr int kDayFont = UI_10_FONT_ID;
constexpr int kSmallFont = SMALL_FONT_ID;
constexpr int kMaxDigitScale = 150;  // percent of the anti-aliased Standby digits

constexpr StrId kWeekdays[] = {StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE,
                               StrId::STR_CAL_WEEKDAY_WED, StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI,
                               StrId::STR_CAL_WEEKDAY_SAT};

bool leapYear(const int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(const int year, const int month /* 1..12 */) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && leapYear(year) ? 29 : kDays[(month - 1) % 12];
}

// 0 = Sunday (Sakamoto).
int weekday(int year, const int month, const int day) {
  static constexpr int kOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) year -= 1;
  return (year + year / 4 - year / 100 + year / 400 + kOffsets[month - 1] + day) % 7;
}

// "周日,周一,..." split on commas into the seven column headings.
void weekHeading(const int column, char* out, const size_t size) {
  const char* text = tr(STR_RICKY_CAL_WEEK_HEADER);
  for (int i = 0; i < column && text; ++i) {
    text = strchr(text, ',');
    if (text) ++text;
  }
  if (!text) {
    out[0] = '\0';
    return;
  }
  const char* end = strchr(text, ',');
  const size_t length = std::min(size - 1, end ? static_cast<size_t>(end - text) : strlen(text));
  memcpy(out, text, length);
  out[length] = '\0';
}

void drawCentered(const GfxRenderer& renderer, const int font, const int centerX, const int y, const char* text,
                  const bool black = true) {
  renderer.drawText(font, centerX - renderer.getTextWidth(font, text) / 2, y, text, black);
}
}  // namespace

StandbyFace::TickResult CalendarClockFace::tick() {
  std::tm local{};
  if (!halClock.localTime(local)) return TickResult::None;
  return local.tm_hour * 60 + local.tm_min == lastMinute_ ? TickResult::None : TickResult::Redraw;
}

uint32_t CalendarClockFace::secondsUntilNextWake() const {
  const uint32_t second = static_cast<uint32_t>(time(nullptr) % 60);
  return second == 0 ? 60u : 60u - second;
}

void CalendarClockFace::render(GfxRenderer& renderer, const Rect& viewport) {
  const int divider = viewport.x + viewport.width / 2;
  const Rect left{viewport.x + kMargin, viewport.y + kMargin, divider - viewport.x - kMargin * 2,
                  viewport.height - kMargin * 2};
  const Rect right{divider + kMargin, viewport.y + kMargin, viewport.x + viewport.width - kMargin - divider - kMargin,
                   viewport.height - kMargin * 2};
  drawClock(renderer, left);
  renderer.drawLine(divider, left.y + kMargin / 2, divider, left.y + left.height - kMargin / 2, 2, true);
  drawCalendar(renderer, right);

  std::tm local{};
  lastMinute_ = halClock.localTime(local) ? local.tm_hour * 60 + local.tm_min : -1;
}

void CalendarClockFace::drawClock(GfxRenderer& renderer, const Rect& area) const {
  std::tm local{};
  const bool known = halClock.localTime(local);
  const bool twelveHour = SETTINGS.clockFormat == 1;
  char clock[8];
  if (!known) {
    snprintf(clock, sizeof(clock), "--:--");
  } else {
    int hour = local.tm_hour;
    if (twelveHour) hour = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(clock, sizeof(clock), "%02d:%02d", hour, local.tm_min);
  }

  // As large as the half allows: "00:00" sets the scale so the digits never jump.
  const int scale = std::min(kMaxDigitScale, area.width * 100 / std::max(1, RickyClockDigits::width("00:00")));
  const int clockWidth = RickyClockDigits::widthBw(clock, scale);
  const int clockHeight = RickyClockDigits::capHeightBw(scale);
  const int clockX = area.x + (area.width - clockWidth) / 2;
  const int clockY = area.y + area.height * 2 / 9;
  RickyClockDigits::drawBw(renderer, clockX, clockY, clock, scale);
  const int dayHeight = renderer.getLineHeight(kDayFont);
  if (known && twelveHour) {
    const char* half = local.tm_hour < 12 ? tr(STR_RICKY_AM) : tr(STR_RICKY_PM);
    renderer.drawText(kDayFont, clockX + clockWidth - renderer.getTextWidth(kDayFont, half),
                      clockY + clockHeight + dayHeight / 2, half, true);
  }

  // Under a short rule, bottom left: battery, then the full date.
  const int smallHeight = renderer.getLineHeight(kSmallFont);
  const int dateY = area.y + area.height - smallHeight;
  const int batteryY = dateY - smallHeight - smallHeight / 2;
  const int ruleY = batteryY - smallHeight;
  renderer.drawLine(area.x, ruleY, area.x + std::min(area.width, 240), ruleY, 1, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawBatteryLeft(renderer, Rect{area.x, batteryY, metrics.batteryWidth, metrics.batteryHeight}, true);
  if (known) {
    char date[64];
    snprintf(date, sizeof(date), tr(STR_RICKY_CAL_FULL_DATE), static_cast<unsigned>(local.tm_year + 1900),
             static_cast<unsigned>(local.tm_mon + 1), static_cast<unsigned>(local.tm_mday),
             I18N.get(kWeekdays[std::clamp(local.tm_wday, 0, 6)]));
    renderer.drawText(kSmallFont, area.x, dateY, date, true);
  }
}

void CalendarClockFace::drawCalendar(GfxRenderer& renderer, const Rect& area) const {
  std::tm local{};
  if (!halClock.localTime(local)) {
    drawCentered(renderer, kSmallFont, area.x + area.width / 2, area.y + area.height / 2, tr(STR_STANDBY_SYNCING));
    return;
  }
  const int year = local.tm_year + 1900;
  const int month = local.tm_mon + 1;

  // Month title right-aligned over a hairline.
  const int titleHeight = renderer.getLineHeight(kTitleFont);
  char title[16];
  snprintf(title, sizeof(title), "%d%s", month, tr(STR_CAL_MONTH_SUFFIX));
  {
    GfxRenderer::SyntheticBoldScope bold(renderer, 2);
    renderer.drawText(kTitleFont, area.x + area.width - renderer.getTextWidth(kTitleFont, title, EpdFontFamily::BOLD),
                      area.y, title, true, EpdFontFamily::BOLD);
  }
  const int ruleY = area.y + titleHeight + titleHeight / 3;
  renderer.drawLine(area.x, ruleY, area.x + area.width, ruleY, 1, true);

  // Weekday headings, then six weeks starting on Sunday; days of the months around
  // this one are smaller, so the month reads as a block without gray.
  const int columnWidth = area.width / 7;
  const int smallHeight = renderer.getLineHeight(kSmallFont);
  const int dayHeight = renderer.getLineHeight(kDayFont);
  const int headingY = ruleY + smallHeight * 2 / 3;
  char text[24];
  for (int column = 0; column < 7; ++column) {
    weekHeading(column, text, sizeof(text));
    drawCentered(renderer, kSmallFont, area.x + columnWidth * column + columnWidth / 2, headingY, text);
  }
  const int gridTop = headingY + smallHeight + smallHeight / 2;
  const int rowHeight = (area.y + area.height - gridTop) / 6;

  const int first = weekday(year, month, 1);
  const int days = daysInMonth(year, month);
  const int previousDays = daysInMonth(month == 1 ? year - 1 : year, month == 1 ? 12 : month - 1);
  for (int cell = 0; cell < 42; ++cell) {
    int day = cell - first + 1;
    const bool inMonth = day >= 1 && day <= days;
    if (day < 1) day += previousDays;
    if (day > days) day -= days;
    snprintf(text, sizeof(text), "%02d", day);
    const int centerX = area.x + columnWidth * (cell % 7) + columnWidth / 2;
    const int rowY = gridTop + rowHeight * (cell / 7);
    if (!inMonth) {
      drawCentered(renderer, kSmallFont, centerX, rowY + (dayHeight - smallHeight) / 2, text);
      continue;
    }
    if (day == local.tm_mday) {
      const int boxWidth = renderer.getTextWidth(kDayFont, text) + 16;
      const int boxHeight = dayHeight + 4;
      renderer.fillRoundedRect(centerX - boxWidth / 2, rowY - 2, boxWidth, boxHeight, 8, Color::Black);
      drawCentered(renderer, kDayFont, centerX, rowY, text, false);
    } else {
      drawCentered(renderer, kDayFont, centerX, rowY, text);
    }
  }
}
