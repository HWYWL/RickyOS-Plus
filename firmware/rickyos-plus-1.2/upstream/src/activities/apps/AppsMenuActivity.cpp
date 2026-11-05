#include "components/RickyGrayStroke.h"
#include "AppsMenuActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "InxItemLayout.h"
#include "components/RickyAaIcons.h"
#include "components/RickyDesignUi.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/inx_apps.h"
#include "components/icons/rickyAppIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

using appVisibility::appBit;
using appVisibility::AppId;

struct AppEntry {
  AppId id;
  StrId titleId;
  UIIcon icon;
  void (ActivityManager::*open)();
};

constexpr AppEntry kAppEntries[] = {
    {AppId::FileTransfer, StrId::STR_FILE_TRANSFER, UIIcon::Transfer, &ActivityManager::goToFileTransfer},
    {AppId::WeRead, StrId::STR_WEREAD_TITLE, UIIcon::WeRead, &ActivityManager::goToWeRead},
    {AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, &ActivityManager::goToRickyReadingStats},
    {AppId::Gomoku, StrId::STR_GOMOKU_TITLE, UIIcon::Gomoku, &ActivityManager::goToGomoku},
    {AppId::Calculator, StrId::STR_CALCULATOR_TITLE, UIIcon::Calculator, &ActivityManager::goToCalculator},
#if FREEINK_DEVICE_METALIO_PLUS
    {AppId::Recorder, StrId::STR_REC_TITLE, UIIcon::Recorder, &ActivityManager::goToRecorder},
#endif
    {AppId::Gallery, StrId::STR_GALLERY_TITLE, UIIcon::Gallery, &ActivityManager::goToGallery},
    // The one entry for Standby & power off (not repeated in Settings).
    {AppId::Standby, StrId::STR_RICKY_POWER_PAGE_TITLE, UIIcon::Wallpaper, &ActivityManager::goToStandbySettings},
};

// Preserve the product's three columns; page capacity follows the Plus viewport.
constexpr int appGridPageStart(int selected, int count, int pageSize) {
  return count > 0 ? std::clamp(selected, 0, count - 1) / pageSize * pageSize : 0;
}
constexpr int kAppCount = static_cast<int>(sizeof(kAppEntries) / sizeof(kAppEntries[0]));

constexpr int visibleAppCount(const uint32_t hiddenMask) {
  int count = 0;
  for (const auto& app : kAppEntries) {
    if ((hiddenMask & appBit(app.id)) == 0) {
      // cppcheck-suppress useStlAlgorithm
      ++count;
    }
  }
  return count;
}

constexpr int appIndexForVisibleIndex(const uint32_t hiddenMask, const int visibleIndex) {
  int visible = 0;
  for (int appIndex = 0; appIndex < kAppCount; ++appIndex) {
    if ((hiddenMask & appBit(kAppEntries[appIndex].id)) != 0) continue;
    if (visible++ == visibleIndex) return appIndex;
  }
  return -1;
}

constexpr bool appIdsAreUnique() {
  for (int i = 0; i < kAppCount; ++i) {
    for (int j = i + 1; j < kAppCount; ++j) {
      if (kAppEntries[i].id == kAppEntries[j].id) return false;
    }
  }
  return true;
}

static_assert(kAppCount <= 32, "the app catalog must fit hiddenAppsMask");
static_assert(static_cast<uint8_t>(AppId::Count) <= 32, "hiddenAppsMask supports at most 32 stable app IDs");
static_assert(appIdsAreUnique(), "stable app IDs must not be reused");
static_assert(visibleAppCount(0) == kAppCount, "a zero mask must show every compiled app");
static_assert(visibleAppCount(UINT32_MAX) == 0, "a full mask must hide every compiled app");
static_assert(visibleAppCount(appBit(AppId::Gomoku)) == kAppCount - 1, "the mask must hide compiled Gomoku");
static_assert(appIndexForVisibleIndex(appBit(kAppEntries[1].id), 1) == 2,
              "visible indices must skip a hidden middle app");

}  // namespace

namespace {
// The product grid draws native 40 px Lucide icons (same system as the tab bar)
// at native size; the stock 32 px pixel icons are only doubled, which looks coarse.
const uint8_t* rickyAppIcon(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Transfer:
      return ricky_app_transfer_40;
    case UIIcon::WeRead:
      return ricky_app_weread_40;
    case UIIcon::ReadingStats:
      return ricky_app_stats_40;
    case UIIcon::Gomoku:
      return ricky_app_gomoku_40;
    case UIIcon::Calculator:
      return ricky_app_calculator_40;
    case UIIcon::Standby:
      return ricky_app_standby_40;
    case UIIcon::Recorder:
      return ricky_app_recorder_40;
    case UIIcon::Gallery:
      return ricky_app_gallery_40;
    case UIIcon::Wallpaper:
      return ricky_app_wallpaper_40;
    default:
      return nullptr;
  }
}

// Every app sits on a rounded-square tile (the Settings cards' corner language) so the
// grid reads as apps rather than loose glyphs; the selected one is filled with the icon
// knocked out, like the selected tab.
constexpr int kAppTileSize = 112;
constexpr int kAppTileRadius = ricky::ds::radius::kControl;
constexpr int kAppTileStroke = ricky::ds::stroke::kHairline;
constexpr int kAppPillWidth = kAppTileSize;  // the tile's footprint in the cell layout
constexpr int kAppPillHeight = kAppTileSize;

int appGridPageSize(const Rect& content, const int visibleCount) {
#if FREEINK_DEVICE_METALIO_PLUS
  // Tile, label and selection padding must fit without touching the next row.
  constexpr int minCellHeight = kAppPillHeight + 10 + 28 + 48;
  const int gap = ricky::ds::space::kGutter;
  const int availableRows = std::max(1, (content.height + gap) / (minCellHeight + gap));
  const int neededRows = (std::max(1, visibleCount) + 2) / 3;
  return 3 * std::min(availableRows, neededRows);
#else
  (void)content;
  (void)visibleCount;
  return 6;
#endif
}

void drawRickyAppIcon(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y, const bool selected) {
  const int tileX = x + (kRickyAppIconSize - kAppTileSize) / 2;
  const int tileY = y + (kRickyAppIconSize - kAppTileSize) / 2;
  if (selected) {
    renderer.fillRoundedRect(tileX, tileY, kAppTileSize, kAppTileSize, kAppTileRadius, Color::Black);
  } else {
    RickyGrayStroke::ring(renderer, tileX, tileY, kAppTileSize, kAppTileSize, kAppTileStroke, kAppTileRadius,
                          ricky::ds::gray::kLine);
  }
  if (RickyAaIcons::draw(renderer, icon, kRickyAppIconSize, kRickyAppIconSize, x, y, !selected)) return;
  constexpr int rowBytes = (kRickyAppIconSize + 7) / 8;
  for (int row = 0; row < kRickyAppIconSize; ++row) {
    for (int column = 0; column < kRickyAppIconSize; ++column) {
      if ((icon[row * rowBytes + column / 8] & (0x80U >> (column % 8))) == 0)
        renderer.drawPixel(x + column, y + row, !selected);
    }
  }
}
}  // namespace

int AppsMenuActivity::getAppCount() { return kAppCount; }

StrId AppsMenuActivity::getAppTitleId(const int appIndex) {
  return appIndex >= 0 && appIndex < kAppCount ? kAppEntries[appIndex].titleId : StrId::STR_NONE_OPT;
}

bool AppsMenuActivity::isAppVisible(const int appIndex) {
  if (appIndex < 0 || appIndex >= kAppCount) return false;
  return (SETTINGS.hiddenAppsMask & appBit(kAppEntries[appIndex].id)) == 0;
}

bool AppsMenuActivity::setAppVisible(const int appIndex, const bool visible) {
  if (appIndex < 0 || appIndex >= kAppCount) return false;

  const uint32_t bit = appBit(kAppEntries[appIndex].id);
  const uint32_t updatedMask = visible ? SETTINGS.hiddenAppsMask & ~bit : SETTINGS.hiddenAppsMask | bit;
  if (updatedMask == SETTINGS.hiddenAppsMask) return false;

  SETTINGS.hiddenAppsMask = updatedMask;
  return true;
}

int AppsMenuActivity::getVisibleAppCount() { return visibleAppCount(SETTINGS.hiddenAppsMask); }

void AppsMenuActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  nav.selected = MainTabs::contentEdgeIndex(edge, getVisibleAppCount());
  nav.follow(getVisibleAppCount());
}

int AppsMenuActivity::getAppIndexForVisibleIndex(const int visibleIndex) {
  return appIndexForVisibleIndex(SETTINGS.hiddenAppsMask, visibleIndex);
}

void AppsMenuActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRowItems();
}

void AppsMenuActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(static_cast<size_t>(getVisibleAppCount()));
  for (int visibleIndex = 0; visibleIndex < getVisibleAppCount(); ++visibleIndex) {
    const int appIndex = getAppIndexForVisibleIndex(visibleIndex);
    if (appIndex < 0) continue;
    fui::ListItem item;
    item.label = I18N.get(kAppEntries[appIndex].titleId);
    item.icon = listIconFor(kAppEntries[appIndex].icon, 32);
    item.actionValue = static_cast<int16_t>(visibleIndex);
    rowItems.push_back(item);
  }
}

bool AppsMenuActivity::usesIconLayout() const {
  return UITheme::getInstance().hasMainTabs() &&
         InxGridGeometry::layoutFrom(SETTINGS.inxAppsLayout) == InxItemLayout::Icons;
}

Rect AppsMenuActivity::appContentRect() const {
  const int spacing = UITheme::getInstance().getMetrics().verticalSpacing;
  Rect content = pageContentRect();
  content.y += spacing;
  content.height -= spacing * 2;
  content.x += ricky::ds::space::kPageMargin;
  content.width -= ricky::ds::space::kPageMargin * 2;
  content.y += ricky::ds::layout::kPageHeaderHeight;
  content.height -= ricky::ds::layout::kPageHeaderHeight;
#if !FREEINK_DEVICE_METALIO_PLUS
  content.height = std::min(content.height, 448);
#endif
  return content;
}

int AppsMenuActivity::iconIndexFromPoint(const int x, const int y) const {
  const Rect content = appContentRect();
  const int visibleCount = getVisibleAppCount();
  const int pageSize = appGridPageSize(content, visibleCount);
  const int start = appGridPageStart(nav.selected, visibleCount, pageSize);
  for (int slot = 0; slot < pageSize && start + slot < visibleCount; ++slot) {
    const auto rect = RickyPageLayout::cell(content, slot, pageSize, ricky::ds::space::kGutter, 3);
    if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) return start + slot;
  }
  return -1;
}

void AppsMenuActivity::openSelected() {
  const int appIndex = getAppIndexForVisibleIndex(nav.selected);
  if (appIndex >= 0) (activityManager.*kAppEntries[appIndex].open)();
}

void AppsMenuActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

bool AppsMenuActivity::handleCustomInput() {
  if (!usesIconLayout()) return false;

  const int visibleCount = getVisibleAppCount();
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    const int touched = iconIndexFromPoint(x, y);
    if (touched >= 0 && touched != nav.selected) {
      nav.selected = touched;
      requestUpdate();
    }
    return true;
  }
  if (mappedInput.wasScreenTapped(x, y)) {
    const int touched = iconIndexFromPoint(x, y);
    if (touched >= 0) {
      nav.selected = touched;
      openSelected();
    }
    return true;
  }
  const auto swipe = mappedInput.wasSwipe();
  const int pageSize = appGridPageSize(appContentRect(), visibleCount);
  if (visibleCount <= pageSize &&
      (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
    return true;  // One page: a swipe must not move focus or cause an e-ink refresh.
  }
  if (swipe == MappedInputManager::SwipeDir::Up) {
    nav.selected = ButtonNavigator::nextPageIndex(nav.selected, visibleCount, pageSize);
    requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    nav.selected = ButtonNavigator::previousPageIndex(nav.selected, visibleCount, pageSize);
    requestUpdate();
    return true;
  }
  return false;
}

void AppsMenuActivity::drawIconGrid(const Rect& rect, const int visibleCount, const bool showSelection) const {
  const int pageSize = appGridPageSize(rect, visibleCount);
  const int start = appGridPageStart(nav.selected, visibleCount, pageSize);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  for (int slot = 0; slot < pageSize && start + slot < visibleCount; ++slot) {
    const int visibleIndex = start + slot;
    const int appIndex = getAppIndexForVisibleIndex(visibleIndex);
    if (appIndex < 0) continue;
    const Rect cell = RickyPageLayout::cell(rect, slot, pageSize, ricky::ds::space::kGutter, 3);
    const bool isSelected = showSelection && visibleIndex == nav.selected;
    const std::string label =
        renderer.truncatedText(UI_10_FONT_ID, I18N.get(kAppEntries[appIndex].titleId), std::max(1, cell.width - 8));
    const int labelX = cell.x + (cell.width - renderer.getTextWidth(UI_10_FONT_ID, label.c_str())) / 2;
    if (const uint8_t* icon = rickyAppIcon(kAppEntries[appIndex].icon)) {
      // Lay out the capsule's height so a selected tile never crowds its label.
      constexpr int labelGap = 10;
      const int blockTop = cell.y + std::max(8, (cell.height - kAppPillHeight - labelGap - lineHeight) / 2);
      // One continuous selection surface, including the icon and its name.
      // The 152 px design footprint shrinks only when the oriented cell needs it.
      const int blockWidth = std::min(152, cell.width - 16);
      if (isSelected)
        RickyDesignUi::fill(
            renderer,
            {static_cast<int16_t>(cell.x + (cell.width - blockWidth) / 2), static_cast<int16_t>(blockTop - 12),
             static_cast<int16_t>(blockWidth), static_cast<int16_t>(kAppPillHeight + labelGap + 28 + 22)},
            ricky::ds::gray::kInk, ricky::ds::radius::kControl);
      const int iconX = cell.x + (cell.width - kRickyAppIconSize) / 2;
      const int iconY = blockTop + (kAppPillHeight - kRickyAppIconSize) / 2;
      drawRickyAppIcon(renderer, icon, iconX, iconY, isSelected);
      const fui::Rect labelRect{static_cast<int16_t>(cell.x + 8),
                                static_cast<int16_t>(blockTop + kAppPillHeight + labelGap),
                                static_cast<int16_t>(cell.width - 16), 28};
      RickyDesignUi::text(renderer, labelRect, I18N.get(kAppEntries[appIndex].titleId), RickyDesignUi::CAPTION,
                          isSelected, fui::TextAlign::Center);
      continue;
    }
    const int iconScale = cell.height >= InxAppIcons::size * 2 + lineHeight + 18 ? 2 : 1;
    const int iconSize = InxAppIcons::size * iconScale;
    if (isSelected) renderer.fillRect(cell.x, cell.y, cell.width, cell.height, true);

    const int iconX = cell.x + (cell.width - iconSize) / 2;
    const int iconY = cell.y + std::max(5, (cell.height - iconSize - lineHeight - 8) / 2);
    InxAppIcons::draw(renderer, kAppEntries[appIndex].icon, iconX, iconY, iconScale, isSelected);
    renderer.drawText(UI_10_FONT_ID, labelX, iconY + iconSize + 8, label.c_str(), !isSelected);
  }

  if (visibleCount > pageSize) GUI.drawSideScrollBar(renderer, rect, visibleCount, start, pageSize);
}

void AppsMenuActivity::buildScreen(UiScreen& screen) {
  if (usesMainTabBar()) {
    const auto root = pageContentRect();
    RickyDesignUi::text(
        renderer,
        {ricky::ds::space::kPageMargin, static_cast<int16_t>(root.y),
         static_cast<int16_t>(root.width - ricky::ds::space::kPageMargin * 2), ricky::ds::layout::kPageHeaderHeight},
        tr(STR_APPS_TITLE), RickyDesignUi::TITLE);
  }
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const Rect content = appContentRect();
  const int visibleCount = getVisibleAppCount();
  const bool showSelection = showMainTabContentSelection();

  if (visibleCount == 0) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_NO_APPS_ENABLED), 2);
  } else if (usesIconLayout()) {
    drawIconGrid(content, visibleCount, showSelection);
  } else {
    screen.setContentMarginFromScreen(
        fui::Insets{static_cast<int16_t>(content.y), static_cast<int16_t>(sw - content.x - content.width),
                    static_cast<int16_t>(sh - content.y - content.height), static_cast<int16_t>(content.x)});
    fui::ListProps props;
    props.items = rowItems.data();
    props.count = static_cast<uint16_t>(rowItems.size());
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    syncListViewport(screen, props);
    screen.list(props);
  }
}

void AppsMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_APPS_TITLE));
}

void AppsMenuActivity::drawFooter() {
  const int visibleCount = getVisibleAppCount();
  const auto labels = mainTabButtonLabels(tr(STR_BACK), tr(STR_SELECT), visibleCount > 1);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AppsMenuActivity::onBackButton() { activityManager.goHome(); }
