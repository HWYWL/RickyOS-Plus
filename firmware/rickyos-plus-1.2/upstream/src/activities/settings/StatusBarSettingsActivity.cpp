#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderChrome.h"
#include "components/RickyDesignUi.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_CHAPTER_PAGE_COUNT = 0,
  ITEM_BOOK_PROGRESS_PERCENTAGE,
  ITEM_PROGRESS_BAR,
  ITEM_PROGRESS_BAR_THICKNESS,
  ITEM_TITLE,
  ITEM_BATTERY,
  ITEM_CLOCK,
  ITEM_COUNT
};

constexpr int FULL_MENU_ITEMS = ITEM_COUNT;
static_assert(FULL_MENU_ITEMS == StatusBarSettingsActivity::MAX_STATUS_BAR_ITEMS,
              "keep StatusBarSettingsActivity::MAX_STATUS_BAR_ITEMS in sync with ITEM_COUNT");

const StrId menuNames[FULL_MENU_ITEMS] = {
    StrId::STR_CHAPTER_PAGE_COUNT,
    StrId::STR_BOOK_PROGRESS_PERCENTAGE,
    StrId::STR_PROGRESS_BAR,
    StrId::STR_PROGRESS_BAR_THICKNESS,
    StrId::STR_TITLE,
    StrId::STR_BATTERY,
    StrId::STR_CLOCK,
};

constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int TITLE_ITEMS = 3;
const StrId titleNames[TITLE_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int STATUS_BAR_CLOCK_ITEMS = CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT;
const StrId statusBarClockNames[STATUS_BAR_CLOCK_ITEMS] = {StrId::STR_HIDE, StrId::STR_DIR_RIGHT, StrId::STR_DIR_LEFT};

const int verticalPreviewTextPadding = 40;

// The preview is a small reader page: the reader's own header and footer drawn into a
// card, with gray bars for the text between them, so it shows exactly what reading shows.
constexpr int kPreviewCardHeight = 220;
constexpr int kPreviewSide = 24;
constexpr int kPreviewLabelGap = 36;

Rect previewCard(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - kPreviewCardHeight;
  return Rect{kPreviewSide, y, renderer.getScreenWidth() - 2 * kPreviewSide, kPreviewCardHeight};
}
}  // namespace

StatusBarSettingsActivity::StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("StatusBarSettings", renderer, mappedInput) {}

void StatusBarSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  visibleItemCount = FULL_MENU_ITEMS;

  // Clamp statusBarProgressBar and statusBarTitle in case of corrupt/migrated data
  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarProgressBarThickness = CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarTitle >= TITLE_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  }

  if (SETTINGS.statusBarClock >= STATUS_BAR_CLOCK_ITEMS) {
    SETTINGS.statusBarClock = CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
  }

  // Labels never change (unlike the values, which track live SETTINGS
  // state), so they're set once here rather than every buildScreen() call.
  for (int i = 0; i < visibleItemCount; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

bool StatusBarSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void StatusBarSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  nav.selected = index;
  // Activation opens a popup/sub-activity or repaints a new value; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
  requestUpdate();
}

void StatusBarSettingsActivity::handleSelection() {
  switch (nav.selected) {
    case ITEM_CHAPTER_PAGE_COUNT:
      SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
      break;
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
      break;
    case ITEM_PROGRESS_BAR:
      optionPopup.show(StrId::STR_PROGRESS_BAR, progressBarNames, PROGRESS_BAR_ITEMS, SETTINGS.statusBarProgressBar,
                       [this](int idx) {
                         SETTINGS.statusBarProgressBar = idx;
                         SETTINGS.saveToFile();
                       });
      return;
    case ITEM_PROGRESS_BAR_THICKNESS:
      optionPopup.show(StrId::STR_PROGRESS_BAR_THICKNESS, progressBarThicknessNames, PROGRESS_BAR_THICKNESS_ITEMS,
                       SETTINGS.statusBarProgressBarThickness, [this](int idx) {
                         SETTINGS.statusBarProgressBarThickness = idx;
                         SETTINGS.saveToFile();
                       });
      return;
    case ITEM_TITLE:
      optionPopup.show(StrId::STR_TITLE, titleNames, TITLE_ITEMS, SETTINGS.statusBarTitle, [this](int idx) {
        SETTINGS.statusBarTitle = idx;
        SETTINGS.saveToFile();
      });
      return;
    case ITEM_BATTERY:
      SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
      break;
    case ITEM_CLOCK:
      SETTINGS.statusBarClock = (SETTINGS.statusBarClock + 1) % STATUS_BAR_CLOCK_ITEMS;
      break;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

std::string StatusBarSettingsActivity::rowValueText(const int index) {
  switch (index) {
    case ITEM_PROGRESS_BAR:
      return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
    case ITEM_PROGRESS_BAR_THICKNESS:
      return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
    case ITEM_TITLE:
      return I18N.get(titleNames[SETTINGS.statusBarTitle]);
    case ITEM_CLOCK:
      return I18N.get(statusBarClockNames[SETTINGS.statusBarClock]);
    default:
      return "";
  }
}

void StatusBarSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Reserve the bottom band for the live status-bar preview footer (label +
  // bar) so the list never runs underneath it, plus the button-hints row below.
  // The preview is pinned directly above the hints (see render()), so the band
  // is just the bar + its label, not a floating gap.
  const auto previewFooter =
      static_cast<int16_t>(kPreviewCardHeight + kPreviewLabelGap + verticalPreviewTextPadding + metrics.verticalSpacing);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight + previewFooter), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_'s labels/actionValue were set once in onEnter(); only the live
  // value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no array growth) rather than building a new
  // items/values vector on every render.
  for (int i = 0; i < visibleItemCount; i++) {
    rowValues_[i] = rowValueText(i);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  GUI.setCheckboxRow(rowItems_[ITEM_CHAPTER_PAGE_COUNT], SETTINGS.statusBarChapterPageCount);
  GUI.setCheckboxRow(rowItems_[ITEM_BOOK_PROGRESS_PERCENTAGE], SETTINGS.statusBarBookProgressPercentage);
  GUI.setCheckboxRow(rowItems_[ITEM_BATTERY], SETTINGS.statusBarBattery);

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(visibleItemCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  props.labelText = UITheme::getInstance().hasMainTabs() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;  // also the explicitly-set marker, see SettingsActivity
  syncListViewport(screen, props);
  screen.list(props);
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the list renders through the app; the preview stays raw.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOMISE_STATUS_BAR));

  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  std::string title;
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = tr(STR_EXAMPLE_BOOK);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    // The reader names both when the chapter has its own title.
    title = std::string(tr(STR_EXAMPLE_BOOK)) + " · " + tr(STR_EXAMPLE_CHAPTER);
  }

  // A small reader page above the button hints: frame, text bars, then the chrome.
  const Rect card = previewCard(renderer, metrics);
  renderer.drawRoundedRect(card.x, card.y, card.width, card.height, ricky::ds::stroke::kHairline,
                           ricky::ds::radius::kControl, true);
  const Rect page{card.x + 4, card.y + 4, card.width - 8, card.height - 8};
  const auto sb = SETTINGS.statusBarSpec();
  const auto chrome = readerChrome::compactLayout(
      std::max(renderer.getLineHeight(RickyDesignUi::CAPTION), renderer.getLineHeight(BaseTheme::STATUS_NUMERIC_FONT_ID)),
      sb.headerLaneVisible(), sb.footerLaneVisible(), sb.showsProgressBar(), sb.progressBarHeightPx);
  constexpr int kTextInset = 20, kBarPitch = 26, kBarHeight = 10;
  const int textTop = page.y + std::max(kTextInset, chrome.headerReserve);
  const int textBottom = page.y + page.height - std::max(kTextInset, chrome.footerReserve);
  for (int y = textTop, row = 0; y + kBarHeight <= textBottom; y += kBarPitch, ++row) {
    const int inset = row % 4 == 3 ? page.width / 3 : 0;  // a paragraph's short last line
    RickyDesignUi::fill(renderer,
                        {static_cast<int16_t>(page.x + kTextInset), static_cast<int16_t>(y),
                         static_cast<int16_t>(page.width - 2 * kTextInset - inset), static_cast<int16_t>(kBarHeight)},
                        ricky::ds::gray::kLine, kBarHeight / 2);
  }
  GUI.drawRickyReaderChrome(renderer, 75, 8, 32, title.c_str(), false, false, true, &page);

  renderer.drawCenteredText(UI_10_FONT_ID, card.y - kPreviewLabelGap, tr(STR_PREVIEW));

  renderer.displayBuffer();
}
