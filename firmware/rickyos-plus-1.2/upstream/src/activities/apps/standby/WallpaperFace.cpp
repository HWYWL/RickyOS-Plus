#include "WallpaperFace.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/RickyBrandMark.h"
#include "components/RickyClockDigits.h"
#include "components/RickyPowerLayout.h"
#include "components/UITheme.h"
#include "components/UiHighDpiProfile.h"
#include "fontIds.h"

namespace {
// Same file the Settings picker installs and the power-off screen shows.
constexpr char kPicture[] = "/sleep.bmp";
constexpr int kCornerMargin = 28;
constexpr int kCornerPadding = 20;
constexpr int kCornerRadius = 18;
constexpr int kTimeFont = NOTOSANS_18_FONT_ID;  // largest built-in face; drawn bold
constexpr int kDateFont = UI_10_FONT_ID;

bool showsTime() { return SETTINGS.standbyOverlay == CrossPointSettings::STANDBY_OVERLAY_TIME; }
bool showsCorner() { return SETTINGS.standbyOverlay != CrossPointSettings::STANDBY_OVERLAY_NONE; }

bool filtered() { return SETTINGS.sleepScreenCoverFilter != CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER; }

struct Placement {
  int x = 0;
  int y = 0;
  float cropX = 0;
  float cropY = 0;
};

// Centered; a larger picture fits the screen, or fills it with the overflow cropped
// (SleepActivity's calculateBitmapPlacement, so power-off shows it the same way).
// Smaller pictures are not enlarged.
Placement placeBitmap(const Bitmap& bitmap, const int screenWidth, const int screenHeight) {
  Placement placement;
  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  if (width <= screenWidth && height <= screenHeight) {
    placement.x = (screenWidth - width) / 2;
    placement.y = (screenHeight - height) / 2;
    return placement;
  }
  const bool fill = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  float ratio = static_cast<float>(width) / static_cast<float>(height);
  const float screenRatio = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
  if (ratio > screenRatio) {
    if (fill) {
      placement.cropX = 1.0f - screenRatio / ratio;
      ratio = (1.0f - placement.cropX) * static_cast<float>(width) / static_cast<float>(height);
    }
    placement.y = static_cast<int>(std::lround((screenHeight - screenWidth / ratio) / 2));
  } else {
    if (fill) {
      placement.cropY = 1.0f - ratio / screenRatio;
      ratio = static_cast<float>(width) / ((1.0f - placement.cropY) * static_cast<float>(height));
    }
    placement.x = static_cast<int>(std::lround((screenWidth - screenHeight * ratio) / 2));
  }
  return placement;
}

// The cover of the book being read as a BMP (made once, then kept with the book's
// cache, as the power-off cover is), or "" when there is no book or no cover.
std::string currentCoverBmp() {
  const std::string& book = APP_STATE.openEpubPath;
  if (book.empty()) return {};
  const bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  if (FsHelpers::hasReflowableBookExtension(book)) {
    Epub epub(book, "/.crosspoint");
    if (!epub.load(true, true) || !epub.generateCoverBmp(cropped)) return {};
    return epub.getCoverBmpPath(cropped);
  }
  return {};
}
}  // namespace

void WallpaperFace::onEnter() {
  path_.clear();
  cardOn_ = SETTINGS.rickyStandbyFace == CrossPointSettings::RICKY_STANDBY_COVER &&
            SETTINGS.standbyCoverLayout != CrossPointSettings::STANDBY_COVER_PICTURE && card_.load();
  if (SETTINGS.rickyStandbyFace == CrossPointSettings::RICKY_STANDBY_COVER) path_ = currentCoverBmp();
  // No book (or no cover): the picture, as the power-off cover falls back to it.
  if (path_.empty() || !Storage.exists(path_.c_str())) path_ = kPicture;
  hasPicture_ = Storage.exists(path_.c_str());
  lastMinute_ = -1;
  lastDay_ = -1;
  grayShown_ = false;
}

StandbyFace::TickResult WallpaperFace::tick() {
  if (!showsCorner()) return TickResult::None;
  std::tm local{};
  if (!halClock.localTime(local)) return TickResult::None;
  const int32_t minute = local.tm_hour * 60 + local.tm_min;
  const bool dayChanged = local.tm_yday != lastDay_;
  if (!showsTime() && !dayChanged) return TickResult::None;
  if (showsTime() && minute == lastMinute_ && !dayChanged) return TickResult::None;
  // Claim the minute now: the 16-gray redraw takes ~2 s, and the loop ticking again
  // before it lands queued a second full redraw of the same minute.
  lastMinute_ = minute;
  lastDay_ = local.tm_yday;
  return TickResult::Redraw;
}

uint32_t WallpaperFace::secondsUntilNextWake() const {
  const time_t now = time(nullptr);
  if (showsTime()) {
    const uint32_t second = static_cast<uint32_t>(now % 60);
    return second == 0 ? 60u : 60u - second;
  }
  if (showsCorner()) {
    std::tm local{};
    if (halClock.localTime(local)) {
      return static_cast<uint32_t>(std::max(1, 86400 - (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec)));
    }
  }
  return 3600u;
}

bool WallpaperFace::wantsClock() const { return showsCorner(); }

void WallpaperFace::drawCorner(GfxRenderer& renderer, const Rect& viewport, const bool intoGray) const {
  if (!showsCorner()) return;
  std::tm local{};
  if (!halClock.localTime(local)) return;  // nothing to show until the clock is set

  static constexpr StrId weekdays[] = {
      StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE, StrId::STR_CAL_WEEKDAY_WED,
      StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI, StrId::STR_CAL_WEEKDAY_SAT};
  char date[48];
  snprintf(date, sizeof(date), tr(STR_RICKY_HOME_DATE), static_cast<unsigned>(local.tm_mon + 1),
           static_cast<unsigned>(local.tm_mday), I18N.get(weekdays[std::clamp(local.tm_wday, 0, 6)]));
  char clock[8];
  snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);

  const bool time = showsTime();
  if (intoGray) {
    // On the picture: a large light clock centred low on the screen over a soft fade to
    // paper, the date under it. No box: a framed badge in the corner read as pasted on.
    const int bottom = viewport.y + viewport.height;
    const int dateHeight = renderer.getLineHeight(kDateFont);
    const int digitsTop = bottom - 300;
    const int dateTop = time ? digitsTop + RickyClockDigits::capHeight() + 30 : bottom - 150;
    const int fadeTop = (time ? digitsTop : dateTop) - 220;
    RickyClockDigits::fadeToPaper(renderer, viewport.x, fadeTop, viewport.width, fadeTop + 200, bottom, 15);
    if (time) {
      RickyClockDigits::draw(renderer, viewport.x + (viewport.width - RickyClockDigits::width(clock)) / 2, digitsTop,
                             clock);
    }
    const int dateWidth = renderer.getTextWidth(kDateFont, date);
    const int dateX = viewport.x + (viewport.width - dateWidth) / 2;
    renderer.fillRect(dateX - 4, dateTop, dateWidth + 8, dateHeight, false);
    renderer.drawText(kDateFont, dateX, dateTop, date, true);
    renderer.copyBwToGrayscale16(dateX - 4, dateTop, dateWidth + 8, dateHeight, 0);
    return;
  }
  const int timeHeight = time ? renderer.getLineHeight(kTimeFont) : 0;
  const int dateHeight = renderer.getLineHeight(kDateFont);
  // Noto Sans digits are tabular, so "00:00" sizes every minute: a corner-only
  // update always covers the previous minute exactly.
  const int textWidth = std::max(renderer.getTextWidth(kDateFont, date),
                                 time ? renderer.getTextWidth(kTimeFont, "00:00", EpdFontFamily::BOLD) : 0);
  const int width = textWidth + 2 * kCornerPadding;
  const int height = 2 * kCornerPadding + timeHeight + dateHeight;
  const int x = viewport.x + viewport.width - kCornerMargin - width;
  const int y = viewport.y + viewport.height - kCornerMargin - height;

  renderer.fillRoundedRect(x, y, width, height, kCornerRadius, Color::White);
  renderer.drawRoundedRect(x, y, width, height, 2, kCornerRadius, true);
  int textY = y + kCornerPadding;
  if (time) {
    renderer.drawText(kTimeFont, x + kCornerPadding, textY, clock, true, EpdFontFamily::BOLD);
    textY += timeHeight;
  }
  renderer.drawText(kDateFont, x + kCornerPadding, textY, date, true);
  if (intoGray) renderer.copyBwToGrayscale16(x, y, width, height, kCornerRadius);
}

bool WallpaperFace::renderNative(GfxRenderer& renderer, const Rect& viewport) {
  std::tm local{};
  const bool clock = halClock.localTime(local);
  if (cardOn_) {
    if (renderer.getGrayscaleLevels() != 16) return false;
    bool shown = renderer.beginGrayscale16() && card_.drawGray(renderer, viewport);
    if (shown) shown = renderer.commitGrayscale16();
    renderer.cancelGrayscale16();
    if (!shown) return false;
    if (clock) {
      lastMinute_ = local.tm_hour * 60 + local.tm_min;
      lastDay_ = local.tm_yday;
    }
    return true;
  }
  if (!hasPicture_) return false;

  // Every update redraws the whole 16-gray frame: a B/W refresh after a 16-gray
  // frame drives every gray pixel to black or white, which collapsed the picture
  // after the first minute. Once a full frame is shown, the panel diffs the new
  // frame against it and drives only the changed clock/date area (no full flash),
  // with a clean pass over that area every kClockCleanupEvery minutes.
  // A filter is a B/W look: render() draws it.
  if (renderer.getGrayscaleLevels() != 16 || filtered()) return false;
  HalFile file;
  if (!Storage.openFileForRead("STANDBY", path_, file)) {
    hasPicture_ = false;
    return false;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("STANDBY", "Unreadable standby picture");
    hasPicture_ = false;
    return false;
  }
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const Placement at = placeBitmap(bitmap, screenWidth, screenHeight);
  bool shown = renderer.beginGrayscale16() &&
               renderer.drawBitmapGrayscale16(bitmap, at.x, at.y, screenWidth, screenHeight, at.cropX, at.cropY);
  if (shown) {
    drawCorner(renderer, viewport, true);
    constexpr uint8_t kClockCleanupEvery = 30;
    if (grayShown_) renderer.setNextGray16UiRefresh(0, 0, 0, 0, kClockCleanupEvery, kClockCleanupEvery);
    shown = renderer.commitGrayscale16();
  }
  renderer.cancelGrayscale16();
  grayShown_ = shown;
  if (!shown) {
    LOG_ERR("STANDBY", "16-gray standby picture failed");
    return false;
  }
  if (clock) {
    lastMinute_ = local.tm_hour * 60 + local.tm_min;
    lastDay_ = local.tm_yday;
  }
  return true;
}

// B/W path: the empty state, and the picture on panels without native 16-gray.
void WallpaperFace::render(GfxRenderer& renderer, const Rect& viewport) {
  if (cardOn_) {
    card_.drawBw(renderer, viewport);
    std::tm local{};
    if (halClock.localTime(local)) {
      lastMinute_ = local.tm_hour * 60 + local.tm_min;
      lastDay_ = local.tm_yday;
    }
    return;
  }
  if (!hasPicture_) {
    // No picture chosen yet: the RickyOS rest screen (the same one the sleep screen
    // shows by default), so Standby starts out branded instead of empty.
    const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto layout = RickyPowerLayout::fit(viewport, textHeight, ricky::ds::space::kBlockGap);
    RickyBrandMark::draw(renderer, layout.mark, 3);
    UITheme::drawCenteredWrappedText(renderer, layout.message, UI_10_FONT_ID, tr(STR_RICKY_BRAND_TAGLINE), 2);
    drawCorner(renderer, viewport, false);
    std::tm local{};
    if (halClock.localTime(local)) {
      lastMinute_ = local.tm_hour * 60 + local.tm_min;
      lastDay_ = local.tm_yday;
    }
    return;
  }
  HalFile file;
  if (!Storage.openFileForRead("STANDBY", path_, file)) return;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;
  const Placement at = placeBitmap(bitmap, renderer.getScreenWidth(), renderer.getScreenHeight());
  renderer.drawBitmap(bitmap, at.x, at.y, renderer.getScreenWidth(), renderer.getScreenHeight(), at.cropX, at.cropY);
  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }
  drawCorner(renderer, viewport, false);
  std::tm local{};
  if (halClock.localTime(local)) {
    lastMinute_ = local.tm_hour * 60 + local.tm_min;
    lastDay_ = local.tm_yday;
  }
}
