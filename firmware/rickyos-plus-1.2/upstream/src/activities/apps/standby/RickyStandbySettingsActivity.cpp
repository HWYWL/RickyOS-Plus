#include "RickyStandbySettingsActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>

#include "CrossPointSettings.h"
#include "KeepPageFace.h"
#include "StandbyActivity.h"
#include "StandbyGalleryActivity.h"
#include "activities/util/ImageViewerActivity.h"
#include "components/RickyDesignUi.h"
#include "components/RickyGrayStroke.h"
#include "components/RickyPageUi.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PanelTileCache.h"
#include "util/ReadingBackground.h"

namespace {
namespace fui = freeink::ui;
namespace ds = ricky::ds;
using Settings = CrossPointSettings;
using Item = RickyStandbySettingsActivity::Item;
constexpr char kPicture[] = "/sleep.bmp";
constexpr char kPowerOffPicture[] = "/poweroff.bmp";

// Standby styles in the order people pick them; stored values differ (RICKY_STANDBY_FACE).
constexpr uint8_t kStyles[] = {Settings::RICKY_STANDBY_PICTURE, Settings::RICKY_STANDBY_COVER,
                               Settings::RICKY_STANDBY_CALENDAR, Settings::RICKY_STANDBY_KEEP_PAGE,
                               Settings::RICKY_STANDBY_TRANSPARENT};
constexpr StrId kStyleCards[] = {StrId::STR_RICKY_CARD_PICTURE, StrId::STR_RICKY_CARD_COVER,
                                 StrId::STR_RICKY_CARD_CALENDAR, StrId::STR_RICKY_CARD_KEEP,
                                 StrId::STR_RICKY_CARD_STICKER};
static_assert(std::size(kStyles) == Settings::RICKY_STANDBY_FACE_COUNT);
static_assert(std::size(kStyleCards) == std::size(kStyles));
constexpr StrId kInfoChips[] = {StrId::STR_RICKY_STANDBY_INFO_NONE, StrId::STR_RICKY_STANDBY_INFO_DATE,
                                StrId::STR_RICKY_CHIP_TIME};
static_assert(std::size(kInfoChips) == Settings::STANDBY_OVERLAY_COUNT);
constexpr StrId kFitChips[] = {StrId::STR_RICKY_CHIP_WHOLE, StrId::STR_RICKY_CHIP_FILL};
static_assert(std::size(kFitChips) == Settings::SLEEP_SCREEN_COVER_MODE_COUNT);
constexpr StrId kFilterChips[] = {StrId::STR_RICKY_CHIP_ORIGINAL, StrId::STR_RICKY_CHIP_CONTRAST,
                                  StrId::STR_RICKY_CHIP_INVERT};
// The cover face: the cover alone, or the "上次读到这里" card (STANDBY_COVER_LAYOUT order).
constexpr StrId kLayoutChips[] = {StrId::STR_RICKY_LAYOUT_COVER, StrId::STR_RICKY_LAYOUT_EDITORIAL,
                                  StrId::STR_RICKY_LAYOUT_SPLIT};
constexpr StrId kExcerptChips[] = {StrId::STR_RICKY_CHIP_HIDE, StrId::STR_RICKY_CHIP_SHOW};
static_assert(std::size(kFilterChips) == Settings::SLEEP_SCREEN_COVER_FILTER_COUNT);
// Minutes before Standby; SLEEP_TIMEOUT_NEVER_MINUTES means never.
constexpr uint8_t kStandbyMinutes[] = {1, 3, 5, 10, 15, 30, Settings::SLEEP_TIMEOUT_NEVER_MINUTES};
constexpr StrId kStandbyMinuteLabels[] = {
    StrId::STR_RICKY_MINUTES_1,  StrId::STR_RICKY_MINUTES_3,  StrId::STR_RICKY_MINUTES_5, StrId::STR_RICKY_MINUTES_10,
    StrId::STR_RICKY_MINUTES_15, StrId::STR_RICKY_MINUTES_30, StrId::STR_RICKY_NEVER};
static_assert(std::size(kStandbyMinuteLabels) == std::size(kStandbyMinutes));
constexpr StrId kAutoOffLabels[] = {StrId::STR_RICKY_NEVER,   StrId::STR_RICKY_HOURS_1,  StrId::STR_RICKY_HOURS_3,
                                    StrId::STR_RICKY_HOURS_6, StrId::STR_RICKY_HOURS_12, StrId::STR_RICKY_HOURS_24};
static_assert(std::size(kAutoOffLabels) == Settings::RICKY_AUTO_OFF_COUNT);
constexpr StrId kPowerOffLabels[] = {StrId::STR_RICKY_POWER_OFF_SAME, StrId::STR_RICKY_SLEEP_LIGHT,
                                     StrId::STR_RICKY_SLEEP_BLANK, StrId::STR_RICKY_POWER_OFF_CUSTOM};
constexpr StrId kPowerOffCards[] = {StrId::STR_RICKY_CARD_SAME, StrId::STR_RICKY_CARD_DEFAULT,
                                    StrId::STR_RICKY_CARD_BLANK, StrId::STR_RICKY_CARD_PICTURE};
static_assert(std::size(kPowerOffLabels) == Settings::RICKY_POWER_OFF_SCREEN_COUNT);
static_assert(std::size(kPowerOffCards) == std::size(kPowerOffLabels));
constexpr StrId kReadingShades[] = {StrId::STR_RICKY_READING_BG_OFF, StrId::STR_RICKY_READING_BG_LIGHT,
                                    StrId::STR_RICKY_READING_BG_MEDIUM, StrId::STR_RICKY_READING_BG_DEEP};
static_assert(std::size(kReadingShades) == Settings::READING_BACKGROUND_COUNT);
constexpr StrId kReadingPapers[] = {StrId::STR_RICKY_PAPER_PICTURE, StrId::STR_RICKY_PAPER_LINES,
                                    StrId::STR_RICKY_PAPER_GRID, StrId::STR_RICKY_PAPER_DOTS};
static_assert(std::size(kReadingPapers) == Settings::READING_PAPER_COUNT);
// The plate around a card or a tile: the inverted selection shows as this ink margin.
constexpr int kPlate = 8;

int styleIndex() {
  for (size_t i = 0; i < std::size(kStyles); ++i)
    if (kStyles[i] == SETTINGS.rickyStandbyFace) return static_cast<int>(i);
  return 0;
}

int standbyMinutesIndex() {
  for (size_t i = 0; i < std::size(kStandbyMinutes); ++i)
    if (kStandbyMinutes[i] == SETTINGS.sleepTimeoutMinutes) return static_cast<int>(i);
  return -1;
}

// The picture and the cover sit on the screen with the same fit, filter and corner.
bool showsPicture() {
  return SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_PICTURE ||
         SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_COVER;
}
bool coverFace() { return SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_COVER; }
// The cover face drawn as the reading card: fit and filter belong to the full-screen cover.
bool readingCard() { return coverFace() && SETTINGS.standbyCoverLayout != Settings::STANDBY_COVER_PICTURE; }

template <size_t N>
void showOption(OptionPopup& popup, const StrId title, const StrId (&labels)[N], const int current,
                std::function<void(int)> apply) {
  popup.show(title, labels, static_cast<int>(N), current, [apply = std::move(apply)](const int chosen) {
    apply(chosen);
    SETTINGS.saveToFile();
  });
}

void saveSetting(uint8_t& field, const uint8_t value) {
  if (field == value) return;
  field = value;
  SETTINGS.saveToFile();
}

fui::Rect rect(const int x, const int y, const int width, const int height) {
  return {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width),
          static_cast<int16_t>(height)};
}

fui::Rect inset(const fui::Rect& r, const int by) { return rect(r.x + by, r.y + by, r.width - 2 * by, r.height - 2 * by); }

// A chip or a button: ink when chosen, else paper with the light-gray hairline edge the
// Home and Library controls rest with.
void control(const GfxRenderer& renderer, const fui::Rect& r, const bool chosen, const int radius) {
  RickyDesignUi::fill(renderer, r, chosen ? ds::gray::kInk : ds::gray::kPaper, radius);
  if (!chosen) RickyGrayStroke::ring(renderer, r.x, r.y, r.width, r.height, ds::stroke::kHairline, radius, ds::gray::kLine);
}

// A screen-shaped preview's frame: the light-gray cover edge.
void previewFrame(const GfxRenderer& renderer, const fui::Rect& r) {
  RickyGrayStroke::ring(renderer, r.x, r.y, r.width, r.height, ds::stroke::kCoverEdge, 0, ds::gray::kLine);
}

// Key focus on a control that already shows its choice by inversion: an emphasis ring
// around it, so focus and choice never read alike.
void focusRing(const GfxRenderer& renderer, const fui::Rect& r, const int radius) {
  constexpr int gap = 6;
  renderer.drawRoundedRect(r.x - gap, r.y - gap, r.width + 2 * gap, r.height + 2 * gap, ds::stroke::kEmphasis,
                           radius + gap, true);
}

// One line centred in a box: RickyDesignUi::text starts at the top of its box.
void centered(const GfxRenderer& renderer, const fui::Rect& r, const char* value, const int font,
              const bool inverted, const ds::gray::Level tone = ds::gray::kInk) {
  const int height = renderer.getLineHeight(RickyDesignUi::fontFor(renderer, font, value));
  RickyDesignUi::text(renderer, rect(r.x, r.y + (r.height - height) / 2, r.width, height), value, font, inverted,
                      fui::TextAlign::Center, 1, tone);
}

void circle(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const ds::gray::Level tone) {
  RickyDesignUi::fill(renderer, rect(cx - radius, cy - radius, 2 * radius, 2 * radius), tone, radius);
}

void textLines(const GfxRenderer& renderer, const fui::Rect& art, const ds::gray::Level tone) {
  const int pad = art.width / 8;
  for (int y = art.y + pad, i = 0; y + 3 < art.bottom() - pad; y += std::max(8, art.height / 12), ++i)
    RickyDesignUi::fill(renderer, rect(art.x + pad, y, art.width - 2 * pad - (i % 4 == 3 ? art.width / 4 : 0), 3),
                        tone);
}

// A small drawing of each style, standing in for the screen it gives.
void styleArt(const GfxRenderer& renderer, const fui::Rect& art, const uint8_t face) {
  RickyDesignUi::fill(renderer, art, ds::gray::kPaper);
  switch (face) {
    case Settings::RICKY_STANDBY_PICTURE: {
      RickyDesignUi::fill(renderer, art, ds::gray::kTint);
      circle(renderer, art.x + art.width * 2 / 3, art.y + art.height / 4, art.width / 9, ds::gray::kPaper);
      const int ground = art.y + art.height * 3 / 5;
      RickyDesignUi::fill(renderer, rect(art.x, ground, art.width, art.bottom() - ground), ds::gray::kLine);
      RickyDesignUi::fill(renderer, rect(art.x, art.y + art.height * 4 / 5, art.width, art.height / 5),
                          ds::gray::kInk2);
      break;
    }
    case Settings::RICKY_STANDBY_COVER: {
      const fui::Rect book =
          rect(art.x + art.width / 5, art.y + art.height / 10, art.width * 3 / 5, art.height * 4 / 5);
      RickyDesignUi::fill(renderer, book, ds::gray::kTint);
      RickyGrayStroke::ring(renderer, book.x, book.y, book.width, book.height, ds::stroke::kHairline, 0, ds::gray::kLine);
      RickyDesignUi::fill(renderer, rect(book.x + 8, book.y + book.height / 5, book.width - 16, 4), ds::gray::kInk2);
      RickyDesignUi::fill(renderer, rect(book.x + 8, book.y + book.height / 5 + 10, book.width / 2, 4),
                          ds::gray::kInk2);
      break;
    }
    case Settings::RICKY_STANDBY_CALENDAR: {
      centered(renderer, rect(art.x, art.y + art.height / 8, art.width, art.height / 3), "10:24",
               RickyDesignUi::CAPTION, false);
      const int cell = std::max(6, art.width / 9);
      const int left = art.x + (art.width - (5 * cell + 4 * 4)) / 2;
      for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 5; ++col)
          RickyDesignUi::fill(renderer,
                              rect(left + col * (cell + 4), art.y + art.height / 2 + row * (cell + 4), cell, cell),
                              row == 1 && col == 2 ? ds::gray::kInk : ds::gray::kLine);
      break;
    }
    case Settings::RICKY_STANDBY_KEEP_PAGE:
      textLines(renderer, art, ds::gray::kInk2);
      break;
    default:
      textLines(renderer, art, ds::gray::kLine);
      circle(renderer, art.x + art.width * 2 / 3, art.y + art.height * 3 / 4, art.width / 6, ds::gray::kInk);
      break;
  }
}

// A picture file drawn into a box (fitted, or filling it and cropped): 16-gray on a gray frame.
bool drawPicture(const GfxRenderer& renderer, const char* path, const fui::Rect& box, const bool fill) {
  HalFile file;
  if (!Storage.openFileForRead("STANDBY", path, file)) return false;
  Bitmap bitmap(file, !renderer.isGrayscale16Active());
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;
  const float sx = static_cast<float>(box.width) / bitmap.getWidth();
  const float sy = static_cast<float>(box.height) / bitmap.getHeight();
  const float scale = fill ? std::max(sx, sy) : std::min(sx, sy);
  const int width = static_cast<int>(std::lround(bitmap.getWidth() * scale));
  const int height = static_cast<int>(std::lround(bitmap.getHeight() * scale));
  const GfxRenderer::ClipScope clip(renderer, box.x, box.y, box.width, box.height);
  const int x = box.x + (box.width - width) / 2;
  const int y = box.y + (box.height - height) / 2;
  if (renderer.isGrayscale16Active() && bitmap.hasGreyscale())
    return renderer.drawBitmapGrayscale16(bitmap, x, y, width, height);
  return renderer.drawBitmap(bitmap, x, y, width, height, 0, 0);
}

bool isPicture(const Item item) { return item >= Item::Picture0 && item <= Item::Picture3; }
}  // namespace

void RickyStandbySettingsActivity::onEnter() {
  KeepPageFace::recoverSticker();
  reloadPictures();
  rebuildRows();
  UiTabListActivity::onEnter();
}

void RickyStandbySettingsActivity::onExit() {
  strip_.clear();
  strip_.shrink_to_fit();
  UiTabListActivity::onExit();
}

void RickyStandbySettingsActivity::reloadPictures() {
  std::vector<StandbyPictures::Entry> all;
  StandbyPictures::list(StandbyPictures::Filter::All, all);
  pictureCount_ = static_cast<int>(all.size());
  strip_.assign(all.begin(), all.begin() + std::min<int>(kStripTiles, pictureCount_));
  standbySource_ = StandbyPictures::sourceInUse(StandbyPictures::Target::Standby);
  // Keep the picture in use in view: it takes the first tile when it is further in.
  const auto inUse = std::find_if(all.begin(), all.end(),
                                  [this](const StandbyPictures::Entry& entry) { return entry.path == standbySource_; });
  if (inUse != all.end() && inUse - all.begin() >= kStripTiles) strip_.front() = *inUse;
  snprintf(allPictures_, sizeof(allPictures_), tr(STR_RICKY_ALL_PICTURES), static_cast<unsigned>(pictureCount_));
  thumbPending_ = 0;
  thumbAttempted_ = 0;
  // The converted screens may have changed behind this page.
  PanelTileCache::forget(kPicture);
  PanelTileCache::forget(kPowerOffPicture);
  PanelTileCache::forget(ReadingBackground::kPath);
}

const char* RickyStandbySettingsActivity::headerTitle() const { return tr(STR_RICKY_POWER_PAGE_TITLE); }

const char* RickyStandbySettingsActivity::tabLabel(const int index) const {
  if (index == ReadingTab) return tr(STR_RICKY_TAB_READING);
  return index == PowerOffTab ? tr(STR_RICKY_TAB_POWER_OFF) : tr(STR_RICKY_TAB_STANDBY);
}

void RickyStandbySettingsActivity::rebuildRows() {
  itemCount_ = 0;
  auto add = [this](const Item item) { items_[itemCount_++] = item; };
  if (tab_ == ReadingTab) {
    for (int i = 0; i < Settings::READING_PAPER_COUNT; ++i) add(static_cast<Item>(ReadingPaper0 + i));
    for (int i = 0; i < Settings::READING_BACKGROUND_COUNT; ++i) add(static_cast<Item>(ReadingShade0 + i));
    if (SETTINGS.readingPaper == Settings::READING_PAPER_PICTURE) add(ChangeReadingPicture);
    return;
  }
  if (tab_ == PowerOffTab) {
    for (int i = 0; i < 4; ++i) add(static_cast<Item>(PowerOff0 + i));
    if (SETTINGS.rickyPowerOffScreen == Settings::RICKY_POWER_OFF_CUSTOM) add(ChangePowerOffPicture);
    add(AutoPowerOff);
    return;
  }
  const bool transparent = SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_TRANSPARENT;
  if (showsPicture())
    for (int i = 0; i < 3; ++i) add(static_cast<Item>(Info0 + i));
  if (coverFace())
    for (int i = 0; i < 3; ++i) add(static_cast<Item>(Layout0 + i));
  if (readingCard())
    for (int i = 0; i < 2; ++i) add(static_cast<Item>(Excerpt0 + i));
  if ((showsPicture() && !readingCard()) || transparent)
    for (int i = 0; i < 2; ++i) add(static_cast<Item>(Fit0 + i));
  if (showsPicture() && !readingCard())
    for (int i = 0; i < 3; ++i) add(static_cast<Item>(Filter0 + i));
#if !((defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4) || defined(SIMULATOR_DEVICE_EEGO_A4))
  // The A4's top key already enters standby with a short press.
  add(StandbyNow);
#endif
  for (int i = 0; i < 5; ++i) add(static_cast<Item>(Style0 + i));
  if (SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_PICTURE) {
    for (int i = 0; i < static_cast<int>(strip_.size()); ++i) add(static_cast<Item>(Picture0 + i));
    add(AllPictures);
  }
  if (transparent) {
    add(ChooseSticker);
    if (Storage.exists(KeepPageFace::kStickerPath)) add(ClearSticker);
  }
  add(AutoStandby);
}

void RickyStandbySettingsActivity::onTabAction(const int index) {
  if (optionPopup.isActive() || index < 0 || index >= TabCount) return;
  if (tab_ != index) {
    tab_ = index;
    rebuildRows();
    auto& n = activeNav();
    n.selected = 0;  // tab taps land with the tab bar focused
    n.followOnBuild = true;
    requestUpdate();
  }
  app.clearTapFlash();
}

void RickyStandbySettingsActivity::stepTab(const int direction) {
  onTabAction((tab_ + (direction < 0 ? TabCount - 1 : 1)) % TabCount);
}

bool RickyStandbySettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] {
    rebuildRows();
    requestUpdate();
  });
}

bool RickyStandbySettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      activateIndex(ringPos() - 1);
    }
    return true;
  }
  return false;
}

void RickyStandbySettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

void RickyStandbySettingsActivity::loop() {
  UiTabListActivity::loop();
  if (thumbPending_) makePendingThumbnail();
}

void RickyStandbySettingsActivity::makePendingThumbnail() {
  StandbyPictures::Entry entry;
  int width = 0;
  int height = 0;
  bool last = false;
  {
    RenderLock lock(*this);
    if (!thumbPending_) return;
    const int slot = __builtin_ctz(thumbPending_);
    thumbPending_ &= thumbPending_ - 1;
    thumbAttempted_ |= 1u << slot;
    if (slot >= static_cast<int>(strip_.size())) return;
    entry = strip_[static_cast<size_t>(slot)];
    width = thumbWidth_;
    height = thumbHeight_;
    last = thumbPending_ == 0;
  }
  // One picture per loop pass, like the gallery: the decode borrows the framebuffer.
  bool made = false;
  {
    RenderLock lock(*this);
    GfxRenderer::FrameBufferLoan loan(renderer);
    made = StandbyPictures::ensureThumbnail(entry, width, height);
  }
  if (!made) LOG_INF("STANDBY", "no thumbnail: %s", entry.path.c_str());
  if (last) {
    RenderLock lock(*this);
    requestUpdate();
  }
}

// What the screen shows in Standby, or once powered off, at the size of the box.
void RickyStandbySettingsActivity::drawPreview(const Rect& box) const {
  const fui::Rect frame = rect(box.x, box.y, box.width, box.height);
  const bool off = tab_ == PowerOffTab;
  const uint8_t powerOff = std::min<uint8_t>(SETTINGS.rickyPowerOffScreen, std::size(kPowerOffLabels) - 1);
  const bool sameAsStandby = !off || powerOff == Settings::RICKY_POWER_OFF_SAME;
  const bool customPowerOff = off && powerOff == Settings::RICKY_POWER_OFF_CUSTOM;
  const bool picture =
      customPowerOff || (sameAsStandby && SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_PICTURE);
  const fui::Rect inner = inset(frame, ds::stroke::kHairline);
  bool painted = false;
  if (picture) {
    const char* path = customPowerOff ? kPowerOffPicture : kPicture;
    const bool fill = !customPowerOff && SETTINGS.sleepScreenCoverMode == Settings::SLEEP_SCREEN_COVER_MODE::CROP;
    char key[56];
    snprintf(key, sizeof(key), "%s|pv|%d,%d,%d,%d|%d", path, inner.x, inner.y, inner.width, inner.height, fill);
    RickyDesignUi::fill(renderer, inner, ds::gray::kPaper);
    painted = PanelTileCache::draw(renderer, key, path, inner.x, inner.y, inner.width, inner.height,
                                   [&] { return drawPicture(renderer, path, inner, fill); });
  }
  if (!painted) {
    if (picture) {
      RickyDesignUi::fill(renderer, inner, ds::gray::kTint);
      centered(renderer, inner, tr(STR_RICKY_STANDBY_EMPTY), RickyDesignUi::CAPTION, false, ds::gray::kInk2);
    } else if (sameAsStandby) {
      styleArt(renderer, inner, SETTINGS.rickyStandbyFace);
    } else if (powerOff == Settings::RICKY_POWER_OFF_BLANK) {
      RickyDesignUi::fill(renderer, inner, ds::gray::kPaper);
    } else {
      RickyDesignUi::fill(renderer, inner, ds::gray::kPaper);
      centered(renderer, inner, "RickyOS", RickyDesignUi::HEADLINE, false);
    }
  }
  // The time and date sit low on the picture, as Standby shows them.
  std::tm local{};
  if (sameAsStandby && showsPicture() && SETTINGS.standbyOverlay != Settings::STANDBY_OVERLAY_NONE &&
      halClock.localTime(local)) {
    static constexpr StrId weekdays[] = {
        StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE, StrId::STR_CAL_WEEKDAY_WED,
        StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI, StrId::STR_CAL_WEEKDAY_SAT};
    char date[48];
    snprintf(date, sizeof(date), tr(STR_RICKY_HOME_DATE), static_cast<unsigned>(local.tm_mon + 1),
             static_cast<unsigned>(local.tm_mday), I18N.get(weekdays[std::clamp(local.tm_wday, 0, 6)]));
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    const int dateHeight = renderer.getLineHeight(RickyDesignUi::CAPTION);
    const int timeHeight = renderer.getLineHeight(RickyDesignUi::HEADLINE);
    const bool time = SETTINGS.standbyOverlay == Settings::STANDBY_OVERLAY_TIME;
    // A narrow preview wraps the date rather than cut the weekday off.
    const int dateLines = renderer.getTextWidth(RickyDesignUi::CAPTION, date) > inner.width - 16 ? 2 : 1;
    const int blockHeight = dateHeight * dateLines + (time ? timeHeight : 0) + 16;
    const fui::Rect band = rect(inner.x, inner.bottom() - blockHeight - inner.height / 8, inner.width, blockHeight);
    RickyDesignUi::fill(renderer, band, ds::gray::kPaper);
    if (time)
      centered(renderer, rect(band.x, band.y + 8, band.width, timeHeight), clock, RickyDesignUi::HEADLINE, false);
    RickyDesignUi::text(renderer,
                        rect(band.x + 8, band.y + 8 + (time ? timeHeight : 0), band.width - 16, dateHeight * dateLines),
                        date, RickyDesignUi::CAPTION, false, fui::TextAlign::Center, dateLines);
  }
  previewFrame(renderer, frame);
}

void RickyStandbySettingsActivity::drawReadingPreview(const fui::Rect& frame) const {
  const fui::Rect inner = inset(frame, ds::stroke::kHairline);
  const uint8_t shade = SETTINGS.readingBackground;
  const bool picture = SETTINGS.readingPaper == Settings::READING_PAPER_PICTURE;
  bool painted = false;
  if (!picture && shade != Settings::READING_BACKGROUND_OFF) {
    // Ruled paper to the sample text's rows, as the reader rules the real ones.
    RickyDesignUi::fill(renderer, inner, ds::gray::kPaper);
    const int pad = inner.width / 8;
    const int step = std::max(8, inner.height / 12);
    ReadingBackground::TextBox box{inner.x + pad, inner.y + pad, inner.right() - pad, inner.bottom() - pad, step,
                                   3 + std::max(2, step / 4)};
    ReadingBackground::drawPaper(renderer, SETTINGS.readingPaper, shade, box);
    painted = true;
  } else if (shade != Settings::READING_BACKGROUND_OFF && Storage.exists(ReadingBackground::kPath)) {
    char key[64];
    snprintf(key, sizeof(key), "%s|pv|%d,%d,%d,%d|%u", ReadingBackground::kPath, inner.x, inner.y, inner.width,
             inner.height, static_cast<unsigned>(shade));
    painted = PanelTileCache::draw(renderer, key, ReadingBackground::kPath, inner.x, inner.y, inner.width,
                                   inner.height, [&] {
                                     if (!drawPicture(renderer, ReadingBackground::kPath, inner, true)) return false;
                                     if (!renderer.isGrayscale16Active()) return true;
                                     // As the reader draws it: lightened under the text.
                                     for (int y = inner.y; y < inner.bottom(); ++y)
                                       for (int x = inner.x; x < inner.right(); ++x)
                                         renderer.drawGrayscale16Pixel(
                                             x, y, ReadingBackground::lighten(renderer.grayscale16Level(x, y), shade) * 17);
                                     return true;
                                   });
  }
  if (!painted) RickyDesignUi::fill(renderer, inner, ds::gray::kPaper);
  textLines(renderer, inner, ds::gray::kInk);
  previewFrame(renderer, frame);
}

void RickyStandbySettingsActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  buildTabBar(screen);
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(fui::Insets{static_cast<int16_t>(theme.spaceLg), theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = ds::space::kGutter;
  const int chipHeight = ds::layout::kButtonCompactHeight;
  const int captionHeight = renderer.getLineHeight(RickyDesignUi::CAPTION);
  const int headingHeight = renderer.getLineHeight(RickyDesignUi::HEADLINE);
  // Ring position 0 is the tab bar; controls follow.
  const int selected = RickyPageUi::syncNav(activeNav(), itemCount_ + 1) - 1;
  const bool focus = showMainTabContentSelection();
  auto indexOf = [this](const Item item) {
    for (int i = 0; i < itemCount_; ++i)
      if (items_[i] == item) return i;
    return -1;
  };
  auto hit = [&](const fui::Rect& r, const Item item) {
    const int index = indexOf(item);
    if (index >= 0) screen.frame().hit(r, ACTION_ROW, index, fui::InputTouch);
    return index >= 0 && focus && selected == index;
  };

  if (tab_ == ReadingTab) {
    // The picture's button at the bottom, the shade and the paper above it, a sample page
    // on the background filling the rest.
    if (indexOf(ChangeReadingPicture) >= 0) {
      const auto button =
          screen.takeBottom(static_cast<int16_t>(ds::layout::kButtonHeight), static_cast<int16_t>(gap));
      const bool buttonPicked = hit(button, ChangeReadingPicture);
      control(renderer, button, buttonPicked, ds::radius::kControl);
      centered(renderer, button,
               Storage.exists(ReadingBackground::kPath) ? tr(STR_RICKY_CHANGE_PICTURE)
                                                        : tr(STR_RICKY_SELECT_READING_BACKGROUND),
               RickyDesignUi::BODY, buttonPicked);
    }
    const auto chipRow = [&](const StrId title, const StrId* labels, const int count, const Item first,
                             const int chosenIndex) {
      const auto row =
          screen.takeBottom(static_cast<int16_t>(captionHeight + 4 + chipHeight), static_cast<int16_t>(gap));
      RickyDesignUi::text(renderer, rect(row.x, row.y, row.width, captionHeight), I18N.get(title),
                          RickyDesignUi::CAPTION, false, fui::TextAlign::Left, 1, ds::gray::kInk2);
      const int chipWidth = (row.width - 12 * (count - 1)) / count;
      for (int i = 0; i < count; ++i) {
        const fui::Rect chip = rect(row.x + i * (chipWidth + 12), row.y + captionHeight + 4, chipWidth, chipHeight);
        const bool picked = hit(chip, static_cast<Item>(first + i));
        control(renderer, chip, chosenIndex == i, chipHeight / 2);
        centered(renderer, chip, I18N.get(labels[i]), RickyDesignUi::CAPTION, chosenIndex == i);
        if (picked) focusRing(renderer, chip, chipHeight / 2);
      }
    };
    // Taken from the bottom up: the shade sits under the paper.
    chipRow(StrId::STR_RICKY_READING_BG_SHADE, kReadingShades, Settings::READING_BACKGROUND_COUNT, ReadingShade0,
            SETTINGS.readingBackground);
    chipRow(StrId::STR_RICKY_READING_PAPER, kReadingPapers, Settings::READING_PAPER_COUNT, ReadingPaper0,
            SETTINGS.readingPaper);
    const auto top = screen.body();
    const int available = std::max(120, static_cast<int>(top.height) - gap);
    const int previewHeight = available;
    const int previewWidth = std::min<int>(top.width, previewHeight * renderer.getScreenWidth() /
                                                          std::max(1, renderer.getScreenHeight()));
    drawReadingPreview(rect(top.x + (top.width - previewWidth) / 2, top.y, previewWidth, previewHeight));
    return;
  }

  // The bottom row: when Standby starts, or when it powers off.
  const auto bottom = screen.takeBottom(static_cast<int16_t>(ds::layout::kRowHeight), static_cast<int16_t>(gap / 2));
  {
    const Item item = tab_ == PowerOffTab ? AutoPowerOff : AutoStandby;
    const bool picked = hit(bottom, item);
    RickyPageUi::card(target, bottom, picked);
    const char* labelText = tab_ == PowerOffTab ? tr(STR_RICKY_AUTO_OFF) : tr(STR_RICKY_AUTO_STANDBY);
    const char* value = nullptr;
    if (tab_ == PowerOffTab) {
      value = I18N.get(kAutoOffLabels[std::min<int>(SETTINGS.rickyAutoOffIndex, std::size(kAutoOffLabels) - 1)]);
    } else if (const int preset = standbyMinutesIndex(); preset >= 0) {
      value = I18N.get(kStandbyMinuteLabels[preset]);
    } else {
      // A value set before the presets (any minute from 1 to 30).
      snprintf(minutes_, sizeof(minutes_), tr(STR_RICKY_MINUTES_N),
               static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes));
      value = minutes_;
    }
    const int bodyHeight = renderer.getLineHeight(RickyDesignUi::BODY);
    const int middle = bottom.y + bottom.height / 2;
    RickyDesignUi::text(renderer, rect(bottom.x + gap, middle - bodyHeight / 2, bottom.width / 2, bodyHeight),
                        labelText, RickyDesignUi::BODY, picked);
    const int chevronX = bottom.right() - 24 - gap;
    RickyDesignUi::text(renderer,
                        rect(bottom.x + bottom.width / 2, middle - captionHeight / 2,
                             chevronX - gap - bottom.x - bottom.width / 2, captionHeight),
                        value, RickyDesignUi::CAPTION, picked, fui::TextAlign::Right, 1, ds::gray::kInk2);
    RickyPageUi::chevron(target, rect(chevronX, bottom.y, 24, bottom.height), picked);
  }

  // Cards: the styles, or the power-off screens, each a small drawing over its name.
  const int cardCount = tab_ == PowerOffTab ? 4 : 5;
  const int cardGap = tab_ == PowerOffTab ? gap : 12;
  const int cardWidth = (screen.body().width - cardGap * (cardCount - 1)) / cardCount;
  const int artWidth = cardWidth - 2 * kPlate;
  const int artHeight = std::min(artWidth * 6 / 5, std::max(80, screen.body().height / 7));
  const int cardHeight = artHeight + 2 * kPlate + 8 + captionHeight;

  // The strip or the overlay's controls under the cards (Standby only).
  const bool pictureStyle = tab_ == StandbyTab && SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_PICTURE;
  const bool transparent = tab_ == StandbyTab && SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_TRANSPARENT;
  const bool customPowerOff =
      tab_ == PowerOffTab && SETTINGS.rickyPowerOffScreen == Settings::RICKY_POWER_OFF_CUSTOM;
  const int stripCellWidth = (screen.body().width - gap * (kStripTiles - 1)) / kStripTiles;
  const int stripArtWidth = stripCellWidth - 2 * kPlate;
  // Tiles show the middle of each picture at 3:4; the gallery and the preview show it whole.
  const int stripArtHeight = std::min(stripArtWidth * 4 / 3, std::max(96, screen.body().height / 6));
  int underCards = 0;
  if (pictureStyle) underCards = headingHeight + 8 + stripArtHeight + 2 * kPlate;
  if (transparent || customPowerOff) underCards = ds::layout::kButtonHeight;
  if (underCards) {
    const auto under = screen.takeBottom(static_cast<int16_t>(underCards), static_cast<int16_t>(gap));
    if (pictureStyle) {
      const fui::Rect heading = rect(under.x, under.y, under.width, headingHeight);
      RickyDesignUi::text(renderer, heading, tr(STR_RICKY_PICTURES), RickyDesignUi::HEADLINE, false);
      // "All 24 ›": the gallery.
      const int linkWidth = renderer.getTextWidth(RickyDesignUi::CAPTION, allPictures_) + 32;
      const fui::Rect link = rect(heading.right() - linkWidth - 8, heading.y - 8, linkWidth + 8, headingHeight + 16);
      const bool linkPicked = hit(link, AllPictures);
      if (linkPicked) RickyDesignUi::fill(renderer, link, ds::gray::kInk, ds::radius::kControl);
      RickyDesignUi::text(renderer,
                          rect(link.x + 4, heading.y + (headingHeight - captionHeight) / 2, linkWidth - 28,
                               captionHeight),
                          allPictures_, RickyDesignUi::CAPTION, linkPicked, fui::TextAlign::Right, 1,
                          ds::gray::kInk2);
      RickyPageUi::chevron(target, rect(link.right() - 28, heading.y, 24, headingHeight), linkPicked);
      const int tilesY = heading.bottom() + 8;
      if (thumbWidth_ != stripArtWidth || thumbHeight_ != stripArtHeight) thumbAttempted_ = 0;
      thumbWidth_ = stripArtWidth;
      thumbHeight_ = stripArtHeight;
      thumbPending_ = 0;
      if (strip_.empty()) {
        centered(renderer, rect(under.x, tilesY, under.width, stripArtHeight), tr(STR_RICKY_STANDBY_EMPTY),
                 RickyDesignUi::CAPTION, false, ds::gray::kInk2);
      }
      for (int i = 0; i < static_cast<int>(strip_.size()); ++i) {
        const auto& entry = strip_[static_cast<size_t>(i)];
        const fui::Rect cell =
            rect(under.x + i * (stripCellWidth + gap), tilesY, stripCellWidth, stripArtHeight + 2 * kPlate);
        const bool picked = hit(cell, static_cast<Item>(Picture0 + i));
        const bool inUse = entry.path == standbySource_;
        if (picked) RickyDesignUi::fill(renderer, cell, ds::gray::kInk, ds::radius::kControl);
        const fui::Rect art = inset(cell, kPlate);
        const std::string thumb = StandbyPictures::thumbnailPath(entry, stripArtWidth, stripArtHeight);
        char key[48];
        snprintf(key, sizeof(key), "|ss|%d,%d,%d,%d|%d", art.x, art.y, art.width, art.height, picked ? 1 : 0);
        const bool painted =
            PanelTileCache::draw(renderer, thumb + key, thumb, art.x, art.y, art.width, art.height, [&] {
              HalFile file;
              if (!Storage.openFileForRead("STANDBY", thumb.c_str(), file)) return false;
              Bitmap bitmap(file);
              return bitmap.parseHeaders() == BmpReaderError::Ok &&
                     GUI.drawCoverThumbFill(renderer, bitmap, Rect{art.x, art.y, art.width, art.height});
            });
        if (!painted) {
          RickyDesignUi::fill(renderer, art, ds::gray::kTint);
          if (!(thumbAttempted_ & (1u << i))) thumbPending_ |= 1u << i;
        }
        if (!picked) RickyGrayStroke::ring(renderer, art.x, art.y, art.width, art.height, ds::stroke::kCoverEdge, 0, ds::gray::kLine);
        if (inUse) {
          // The picture in use: a check on the lower right, ink on whatever the picture is.
          const int size = std::max(28, art.width / 4);
          const int cx = art.right() - size / 2 - 8;
          const int cy = art.bottom() - size / 2 - 8;
          circle(renderer, cx, cy, size / 2 + 2, ds::gray::kPaper);  // a paper edge parts it from the picture
          circle(renderer, cx, cy, size / 2, ds::gray::kInk);
          const auto paper = fui::Paint::solid(fui::Color::White);
          target.line({static_cast<int16_t>(cx - size / 4), static_cast<int16_t>(cy)},
                      {static_cast<int16_t>(cx - size / 12), static_cast<int16_t>(cy + size / 5)}, 3, paper);
          target.line({static_cast<int16_t>(cx - size / 12), static_cast<int16_t>(cy + size / 5)},
                      {static_cast<int16_t>(cx + size / 4), static_cast<int16_t>(cy - size / 6)}, 3, paper);
        }
      }
    } else {
      // The overlay's picture (choose or clear), or another power-off picture.
      const Item main = transparent ? ChooseSticker : ChangePowerOffPicture;
      const bool clearable = transparent && indexOf(ClearSticker) >= 0;
      const int buttonWidth = clearable ? (under.width - gap) / 2 : under.width;
      const fui::Rect primary = rect(under.x, under.y, buttonWidth, under.height);
      const bool picked = hit(primary, main);
      control(renderer, primary, picked, ds::radius::kControl);
      centered(renderer, primary, transparent ? tr(STR_RICKY_SELECT_TRANSPARENT) : tr(STR_RICKY_CHANGE_PICTURE),
               RickyDesignUi::BODY, picked);
      if (clearable) {
        const fui::Rect clear = rect(under.right() - buttonWidth, under.y, buttonWidth, under.height);
        const bool clearPicked = hit(clear, ClearSticker);
        control(renderer, clear, clearPicked, ds::radius::kControl);
        centered(renderer, clear, tr(STR_RICKY_CLEAR_TRANSPARENT), RickyDesignUi::BODY, clearPicked);
      }
    }
  }

  const auto cards =
      screen.takeBottom(static_cast<int16_t>(headingHeight + 8 + cardHeight), static_cast<int16_t>(gap));
  RickyDesignUi::text(renderer, rect(cards.x, cards.y, cards.width, headingHeight),
                      tab_ == PowerOffTab ? tr(STR_RICKY_POWER_OFF_SCREEN) : tr(STR_RICKY_STANDBY_STYLE),
                      RickyDesignUi::HEADLINE, false);
  for (int i = 0; i < cardCount; ++i) {
    const fui::Rect cell =
        rect(cards.x + i * (cardWidth + cardGap), cards.y + headingHeight + 8, cardWidth, cardHeight);
    const Item item = static_cast<Item>((tab_ == PowerOffTab ? PowerOff0 : Style0) + i);
    const bool picked = hit(cell, item);
    const bool chosen = tab_ == PowerOffTab ? SETTINGS.rickyPowerOffScreen == i : styleIndex() == i;
    if (chosen) RickyDesignUi::fill(renderer, cell, ds::gray::kInk, ds::radius::kControl);
    const fui::Rect art = rect(cell.x + kPlate, cell.y + kPlate, artWidth, artHeight);
    if (tab_ == PowerOffTab) {
      if (i == Settings::RICKY_POWER_OFF_SAME) {
        if (!(SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_PICTURE &&
              PanelTileCache::draw(renderer, std::string(kPicture) + "|card", kPicture, art.x, art.y, art.width,
                                   art.height, [&] { return drawPicture(renderer, kPicture, art, true); })))
          styleArt(renderer, art, SETTINGS.rickyStandbyFace);
      } else if (i == Settings::RICKY_POWER_OFF_CUSTOM) {
        if (!PanelTileCache::draw(renderer, std::string(kPowerOffPicture) + "|card", kPowerOffPicture, art.x, art.y,
                                  art.width, art.height,
                                  [&] { return drawPicture(renderer, kPowerOffPicture, art, true); }))
          styleArt(renderer, art, Settings::RICKY_STANDBY_PICTURE);
      } else {
        RickyDesignUi::fill(renderer, art, ds::gray::kPaper);
        if (i == Settings::RICKY_POWER_OFF_DEFAULT) centered(renderer, art, "RickyOS", RickyDesignUi::CAPTION, false);
      }
    } else {
      styleArt(renderer, art, kStyles[i]);
    }
    if (!chosen) RickyGrayStroke::ring(renderer, art.x, art.y, art.width, art.height, ds::stroke::kHairline, 0, ds::gray::kLine);
    centered(renderer, rect(cell.x, art.bottom() + 8, cell.width, captionHeight),
             I18N.get(tab_ == PowerOffTab ? kPowerOffCards[i] : kStyleCards[i]), RickyDesignUi::CAPTION, chosen);
    if (picked) focusRing(renderer, cell, ds::radius::kControl);
  }

  // The preview, with the corner, fit and filter beside it (Standby) or what it is (power-off).
  const auto top = screen.body();
  const int available = std::max(120, static_cast<int>(top.height) - gap);
  const int previewWidth = std::min<int>(top.width * 2 / 5, available * renderer.getScreenWidth() /
                                                                std::max(1, renderer.getScreenHeight()));
  const int previewHeight =
      std::min(available, previewWidth * renderer.getScreenHeight() / std::max(1, renderer.getScreenWidth()));
  drawPreview(Rect{top.x, top.y, previewWidth, previewHeight});
  const int columnX = top.x + previewWidth + gap + 8;
  const int columnWidth = top.right() - columnX;
  int y = top.y;
  auto chipGroup = [&](const StrId title, const StrId* labels, const int count, const Item first, const int chosen) {
    RickyDesignUi::text(renderer, rect(columnX, y, columnWidth, captionHeight), I18N.get(title),
                        RickyDesignUi::CAPTION, false, fui::TextAlign::Left, 1, ds::gray::kInk2);
    y += captionHeight + 4;
    const int chipWidth = (columnWidth - 12 * (count - 1)) / count;
    for (int i = 0; i < count; ++i) {
      const fui::Rect chip = rect(columnX + i * (chipWidth + 12), y, chipWidth, chipHeight);
      const bool picked = hit(chip, static_cast<Item>(first + i));
      control(renderer, chip, chosen == i, chipHeight / 2);
      centered(renderer, chip, I18N.get(labels[i]), RickyDesignUi::CAPTION, chosen == i);
      if (picked) focusRing(renderer, chip, chipHeight / 2);
    }
    y += chipHeight + 12;
  };
  if (tab_ == StandbyTab) {
    if (showsPicture())
      chipGroup(StrId::STR_RICKY_STANDBY_INFO, kInfoChips, 3, Info0, std::min<int>(SETTINGS.standbyOverlay, 2));
    if (coverFace())
      chipGroup(StrId::STR_RICKY_COVER_LAYOUT, kLayoutChips, 3, Layout0, std::min<int>(SETTINGS.standbyCoverLayout, 2));
    if (readingCard())
      chipGroup(StrId::STR_RICKY_STANDBY_EXCERPT, kExcerptChips, 2, Excerpt0, SETTINGS.standbyShowExcerpt ? 1 : 0);
    if ((showsPicture() && !readingCard()) || transparent)
      chipGroup(StrId::STR_RICKY_PICTURE_FIT, kFitChips, 2, Fit0, std::min<int>(SETTINGS.sleepScreenCoverMode, 1));
    if (showsPicture() && !readingCard())
      chipGroup(StrId::STR_RICKY_PICTURE_FILTER, kFilterChips, 3, Filter0,
                std::min<int>(SETTINGS.sleepScreenCoverFilter, 2));
    if (indexOf(StandbyNow) >= 0) {
      const int buttonY = std::max(y, top.y + previewHeight - chipHeight);
      const fui::Rect now = rect(columnX, buttonY, columnWidth, chipHeight);
      const bool picked = hit(now, StandbyNow);
      control(renderer, now, picked, ds::radius::kControl);
      centered(renderer, now, tr(STR_RICKY_STANDBY_NOW), RickyDesignUi::BODY, picked);
    }
  } else {
    const uint8_t powerOff = std::min<uint8_t>(SETTINGS.rickyPowerOffScreen, std::size(kPowerOffLabels) - 1);
    RickyDesignUi::text(renderer, rect(columnX, y, columnWidth, headingHeight * 2),
                        I18N.get(kPowerOffLabels[powerOff]), RickyDesignUi::HEADLINE, false, fui::TextAlign::Left, 2);
  }
}

void RickyStandbySettingsActivity::openGallery(const StandbyPictures::Target target) {
  app.clearTapFlash();
  startActivityForResultWith<StandbyGalleryActivity>(
      [this](const ActivityResult&) {
        reloadPictures();
        rebuildRows();
        requestUpdate();
      },
      target);
}

void RickyStandbySettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= itemCount_ || optionPopup.isActive()) return;
  app.clearTapFlash();
  activeNav().selected = index + 1;
  const Item item = items_[index];
  if (item >= Info0 && item <= Info2) {
    saveSetting(SETTINGS.standbyOverlay, static_cast<uint8_t>(item - Info0));
  } else if (item >= Fit0 && item <= Fit1) {
    saveSetting(SETTINGS.sleepScreenCoverMode, static_cast<uint8_t>(item - Fit0));
  } else if (item >= Filter0 && item <= Filter2) {
    saveSetting(SETTINGS.sleepScreenCoverFilter, static_cast<uint8_t>(item - Filter0));
  } else if (item >= Layout0 && item <= Layout2) {
    saveSetting(SETTINGS.standbyCoverLayout, static_cast<uint8_t>(item - Layout0));
    rebuildRows();  // fit/filter and the excerpt switch come and go with the layout
  } else if (item >= Excerpt0 && item <= Excerpt1) {
    saveSetting(SETTINGS.standbyShowExcerpt, static_cast<uint8_t>(item - Excerpt0));
  } else if (item >= Style0 && item <= Style4) {
    saveSetting(SETTINGS.rickyStandbyFace, kStyles[item - Style0]);
    rebuildRows();
  } else if (isPicture(item)) {
    // A tile opens its picture full screen: set it from there, or flip on through the gallery.
    std::vector<StandbyPictures::Entry> all;
    StandbyPictures::list(StandbyPictures::Filter::All, all);
    std::vector<std::string> paths;
    paths.reserve(all.size());
    int start = 0;
    const auto& chosen = strip_[static_cast<size_t>(item - Picture0)];
    for (const auto& entry : all) {
      if (entry.path == chosen.path) start = static_cast<int>(paths.size());
      paths.push_back(entry.path);
    }
    startActivityForResultWith<ImageViewerActivity>(
        [this](const ActivityResult&) {
          reloadPictures();
          requestUpdate();
        },
        std::move(paths), start, ImageViewerActivity::WallpaperTarget::Standby);
    return;
  } else if (item >= ReadingPaper0 && item <= ReadingPaper3) {
    const auto paper = static_cast<uint8_t>(item - ReadingPaper0);
    // The picture paper needs a picture: choose it now when there is none yet.
    if (paper == Settings::READING_PAPER_PICTURE && !Storage.exists(ReadingBackground::kPath)) {
      openGallery(StandbyPictures::Target::Reading);
      return;
    }
    saveSetting(SETTINGS.readingPaper, paper);
    // Choosing a paper shows it: at medium when the background was off.
    if (SETTINGS.readingBackground == Settings::READING_BACKGROUND_OFF)
      saveSetting(SETTINGS.readingBackground, Settings::READING_BACKGROUND_MEDIUM);
    ReadingBackground::invalidate();
    rebuildRows();
  } else if (item >= ReadingShade0 && item <= ReadingShade3) {
    const auto shade = static_cast<uint8_t>(item - ReadingShade0);
    // A picture shade needs a picture: choose it now when there is none yet.
    if (shade != Settings::READING_BACKGROUND_OFF && SETTINGS.readingPaper == Settings::READING_PAPER_PICTURE &&
        !Storage.exists(ReadingBackground::kPath)) {
      openGallery(StandbyPictures::Target::Reading);
      return;
    }
    saveSetting(SETTINGS.readingBackground, shade);
    ReadingBackground::invalidate();
  } else if (item >= PowerOff0 && item <= PowerOff3) {
    const auto screen = static_cast<uint8_t>(item - PowerOff0);
    // A picture of its own needs one: choose it now when there is none yet.
    if (screen == Settings::RICKY_POWER_OFF_CUSTOM && !Storage.exists(kPowerOffPicture)) {
      openGallery(StandbyPictures::Target::PowerOff);
      return;
    }
    saveSetting(SETTINGS.rickyPowerOffScreen, screen);
    rebuildRows();
  } else {
    switch (item) {
      case AllPictures:
        openGallery(StandbyPictures::Target::Standby);
        return;
      case ChangePowerOffPicture:
        openGallery(StandbyPictures::Target::PowerOff);
        return;
      case ChangeReadingPicture:
        openGallery(StandbyPictures::Target::Reading);
        return;
      case ChooseSticker:
        openGallery(StandbyPictures::Target::Sticker);
        return;
      case ClearSticker:
        // Remove a stale backup first, so a later entry cannot resurrect an old sticker.
        if ((Storage.exists(KeepPageFace::kStickerBackupPath) && !Storage.remove(KeepPageFace::kStickerBackupPath)) ||
            !Storage.remove(KeepPageFace::kStickerPath))
          GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
        rebuildRows();
        break;
      case StandbyNow:
        startActivityForResultWith<StandbyActivity>([this](const ActivityResult&) { requestUpdate(); });
        return;
      case AutoStandby:
        showOption(optionPopup, StrId::STR_RICKY_AUTO_STANDBY, kStandbyMinuteLabels,
                   std::max(0, standbyMinutesIndex()),
                   [](const int chosen) { SETTINGS.sleepTimeoutMinutes = kStandbyMinutes[chosen]; });
        break;
      case AutoPowerOff:
        showOption(optionPopup, StrId::STR_RICKY_AUTO_OFF, kAutoOffLabels, SETTINGS.rickyAutoOffIndex,
                   [](const int chosen) { SETTINGS.rickyAutoOffIndex = static_cast<uint8_t>(chosen); });
        break;
      default:
        return;
    }
  }
  requestUpdate();
}
