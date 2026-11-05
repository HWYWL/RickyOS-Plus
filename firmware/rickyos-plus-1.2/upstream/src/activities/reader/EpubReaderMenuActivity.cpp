#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <array>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "components/RickyDesignUi.h"
#include "components/RickyPageUi.h"
#include "components/RickyRowList.h"
#include "components/UITheme.h"
#include "components/icons/rickyPageIcons.h"
#include "components/themes/lyra/LyraTheme.h"

namespace fui = freeink::ui;

namespace {
const freeink::Icon& menuRowIcon(EpubReaderMenuActivity::MenuAction action) {
  using Action = EpubReaderMenuActivity::MenuAction;
  switch (action) {
    case Action::NIGHT_MODE:
      return icon_ricky_row_night_32;
    case Action::FRONTLIGHT:
      return icon_ricky_row_light_32;
    case Action::ROTATE_SCREEN:
      return icon_ricky_row_rotate_32;
    case Action::HOLDING_HAND:
      return icon_ricky_row_hand_32;
    case Action::READING_MODE:
      return icon_ricky_row_books_32;
    case Action::READING_LINE:
      return icon_ricky_row_lines_32;
    case Action::FONT_SIZE:
    case Action::TEXT_WEIGHT:
      return icon_ricky_row_text_32;
    case Action::AUTO_PAGE_TURN:
      return icon_ricky_row_timer_32;
    case Action::GO_TO_PERCENT:
      return icon_ricky_row_percent_32;
    case Action::BOOKMARKS:
      return icon_ricky_row_bookmark_32;
    case Action::SYNC:
      return icon_ricky_row_sync_32;
    case Action::GO_HOME:
      return icon_ricky_row_home_32;
    default:
      return icon_ricky_row_file_32;
  }
}
}  // namespace

void EpubReaderMenuActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(
      ACTION_USER,
      [](const fui::ActionEvent&, void* user) {
        static_cast<EpubReaderMenuActivity*>(user)->finishWithAction(MenuAction::GO_HOME);
      },
      this);
}

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const char* chapterTitle, const bool pageBookmarked,
                                               const unsigned bookmarkCount)
    : UiListActivity("EpubReaderMenu", renderer, mappedInput, false, !UiHighDpiProfile::enabled),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(menuItems, hasFootnotes, hasBookmarks);
  snprintf(chapterTitle_, sizeof(chapterTitle_), "%s", chapterTitle ? chapterTitle : "");
  // Truncation must never end on a partial UTF-8 character.
  if (chapterTitle && strlen(chapterTitle) >= sizeof(chapterTitle_)) {
    size_t end = sizeof(chapterTitle_) - 1;
    while (end && (static_cast<unsigned char>(chapterTitle[end]) & 0xc0) == 0x80) --end;
    chapterTitle_[end] = '\0';
  }
  pageBookmarked_ = pageBookmarked;
  snprintf(bookmarkCount_, sizeof(bookmarkCount_), "%u", bookmarkCount);
  snprintf(progressValue, sizeof(progressValue), "%d%%", std::clamp(bookProgressPercent, 0, 100));
  setMenuView(false);
}

// Populates menuRowItems's labels/actionValue from menuItems. Called once
// here since menuItems (and thus which rows exist) never changes after
// construction; buildScreen() only touches the two rows with a live value.
void EpubReaderMenuActivity::buildMenuRowItems() {
  for (size_t i = 0; i < menuView.count; i++) {
    fui::ListItem item;
    const int original = menuView.indices[i];
    item.label =
        original == RickyReaderMenuLayout::More ? tr(STR_RICKY_MORE_ACTIONS) : I18N.get(menuItems[original].labelId);
    if (UiHighDpiProfile::enabled && original >= 0) {
      switch (menuItems[original].action) {
        case MenuAction::SELECT_CHAPTER:
          item.label = tr(STR_RICKY_MENU_CHAPTERS);
          break;
        case MenuAction::TOGGLE_BOOKMARK:
          item.label = pageBookmarked_ ? tr(STR_RICKY_MENU_REMOVE_BOOKMARK) : tr(STR_RICKY_MENU_ADD_BOOKMARK);
          break;
        case MenuAction::BOOKMARKS:
          item.value = bookmarkCount_;
          break;
        case MenuAction::TEXT_SETTINGS:
          item.label = tr(STR_RICKY_MENU_TEXT);
          break;
        case MenuAction::AUTO_PAGE_TURN:
          item.label = tr(STR_RICKY_MENU_AUTO_TURN);
          break;
        case MenuAction::GO_TO_PERCENT:
          item.label = tr(STR_RICKY_MENU_PROGRESS);
          item.value = progressValue;
          break;
        default:
          break;
      }
    }
    item.actionValue = static_cast<int16_t>(i);
    menuRowItems[i] = item;
  }
}

void EpubReaderMenuActivity::setMenuView(bool more) {
  moreView = more;
  menuView = RickyReaderMenuLayout::build<MenuAction>(menuItems, more);
  if (!UiHighDpiProfile::enabled) {
    menuView.quickCount = 0;
    menuView.count = static_cast<uint8_t>(std::min(menuItems.size(), MAX_MENU_ITEMS));
    for (int i = 0; i < menuView.count; ++i) menuView.indices[i] = static_cast<int8_t>(i);
    moreView = false;
  }
  buildMenuRowItems();
  nav.reset();
}

void EpubReaderMenuActivity::buildMenuItems(std::vector<MenuItem>& items, bool hasFootnotes, bool hasBookmarks) {
  items.clear();
  items.reserve(MAX_MENU_ITEMS);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
  if (Frontlight.present()) {
    items.push_back({MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT});
  }
  items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::HOLDING_HAND, StrId::STR_PAGE_TURN_DIRECTION});
  items.push_back({MenuAction::READING_MODE, StrId::STR_READING_MODE});
  items.push_back({MenuAction::READING_LINE, StrId::STR_READING_GUIDE_LINE});
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE});
  items.push_back({MenuAction::TEXT_WEIGHT, StrId::STR_RICKY_WEIGHT});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data =
      MenuResult{-1, pendingOrientation,
                 static_cast<uint8_t>(
                     selectedPageTurnOption == 0 ? 0 : std::array<uint8_t, 5>{0, 1, 3, 6, 12}[selectedPageTurnOption])};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive() || index < 0 || index >= listCount()) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  const int original = menuView.indices[index];
  if (original == RickyReaderMenuLayout::More) {
    RenderLock lock(*this);
    setMenuView(true);
    requestUpdate();
    return;
  }
  const auto selectedAction = menuItems[original].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately. Only the renderer turns;
                       // SETTINGS.orientation stays unchanged so the reader's
                       // result handler still detects the change and reflows.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                       requestUpdate(true);
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::READING_MODE) {
    // Saved now; the reader re-paginates when the menu closes (its frame moves the text).
    optionPopup.show(StrId::STR_READING_MODE, readingModeLabels.data(), static_cast<int>(readingModeLabels.size()),
                     SETTINGS.readingMode < readingModeLabels.size() ? SETTINGS.readingMode : 0, [this](int idx) {
                       SETTINGS.readingMode = static_cast<uint8_t>(idx);
                       SETTINGS.saveToFile();
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::READING_LINE) {
    static_assert(readingGuideLine::kMenuChoices == 7, "one label per reading-line choice");
    optionPopup.show(StrId::STR_READING_GUIDE_LINE, readingLineLabels.data(),
                     static_cast<int>(readingLineLabels.size()),
                     readingGuideLine::menuIndexFor(SETTINGS.readingGuideLineEnabled, SETTINGS.readingGuideLineStyle),
                     [this](int idx) {
                       if (idx < 0 || idx >= readingGuideLine::kMenuChoices) return;
                       SETTINGS.readingGuideLineEnabled = idx == 0 ? 0 : 1;
                       if (idx != 0)
                         SETTINGS.readingGuideLineStyle = static_cast<uint8_t>(readingGuideLine::kMenuStyles[idx]);
                       SETTINGS.saveToFile();
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FONT_SIZE) {
    // Saved now; the reader re-paginates (and offers font acceleration) when the menu closes.
    fontSizes_ = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
    if (fontSizes_.empty()) return;
    fontSizeLabels_.clear();
    fontSizeLabels_.reserve(fontSizes_.size());
    int current = 0;
    const uint8_t snapped = snapToNearestPointSize(fontSizes_, SETTINGS.fontPointSize);
    for (size_t i = 0; i < fontSizes_.size(); ++i) {
      fontSizeLabels_.push_back(std::to_string(fontSizes_[i]) + " pt");
      if (fontSizes_[i] == snapped) current = static_cast<int>(i);
    }
    optionPopup.show(StrId::STR_FONT_SIZE, fontSizeLabels_, current, [this](int idx) {
      if (idx < 0 || idx >= static_cast<int>(fontSizes_.size()) || SETTINGS.fontPointSize == fontSizes_[idx]) return;
      SETTINGS.fontPointSize = fontSizes_[idx];
      SETTINGS.saveToFile();
      requestUpdate();
    });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::TEXT_WEIGHT) {
    // Weight is drawn, not laid out: the reader redraws the page when the menu closes.
    optionPopup.show(StrId::STR_RICKY_WEIGHT, weightLabels.data(), static_cast<int>(weightLabels.size()),
                     SETTINGS.rickyTextWeight % weightLabels.size(), [this](int idx) {
                       SETTINGS.rickyTextWeight = static_cast<uint8_t>(idx);
                       SETTINGS.saveToFile();
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::HOLDING_HAND) {
    // Two choices: a tap switches hands, and the page-turn zones follow on return.
    SETTINGS.pageTurnDirection = SETTINGS.pageTurnDirection == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::NIGHT_MODE) {
    SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FRONTLIGHT) {
    const bool lightOn = !Frontlight.isOn();
    Frontlight.setOn(lightOn);
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  finishWithAction(selectedAction);
}

void EpubReaderMenuActivity::finishWithAction(const MenuAction action) {
  app.clearTapFlash();
  setResult(MenuResult{static_cast<int>(action), pendingOrientation,
                       static_cast<uint8_t>(selectedPageTurnOption == 0
                                                ? 0
                                                : std::array<uint8_t, 5>{0, 1, 3, 6, 12}[selectedPageTurnOption])});
  finish();
}

bool EpubReaderMenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool EpubReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (moreView) {
      RenderLock lock(*this);
      setMenuView(false);
      requestUpdate();
    } else {
      closeCancelled();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(usesUpstreamStyle());
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  if (UiHighDpiProfile::enabled) {
    const fui::Rect home{
        static_cast<int16_t>(safe.x + safe.width - ricky::ds::space::kPageMargin - ricky::ds::layout::kTouchMin),
        static_cast<int16_t>(safe.y + metrics.topPadding + ricky::ds::layout::kStatusBarHeight),
        ricky::ds::layout::kTouchMin, ricky::ds::layout::kTouchMin};
    screen.frame().hit(home, ACTION_USER, 0, fui::InputTouch);
  }

  // Progress summary where the old sub-header band sat.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const fui::Rect band =
      screen.takeTop(UiHighDpiProfile::enabled ? ricky::ds::layout::kSectionHeaderHeight : metrics.tabBarHeight);
  const int16_t pad = screen.theme().headerSidePadding;
  if (UiHighDpiProfile::enabled) {
    // Real chapter title, chapter-relative page count and book percentage.
    // Fixed lanes keep a long chapter from displacing the numeric summary.
    const auto content = band.inset({0, ricky::ds::space::kPageMargin, 0, ricky::ds::space::kPageMargin});
    char pages[48];
    if (totalPages > 0)
      snprintf(pages, sizeof(pages), "%d / %d · %d%%", currentPage, totalPages, bookProgressPercent);
    else
      snprintf(pages, sizeof(pages), "%d%%", bookProgressPercent);
    const int lane = std::min<int>(content.width / 2, renderer.getTextWidth(RickyDesignUi::CAPTION, pages) + 16);
    RickyDesignUi::secondaryText(
        renderer, {content.x, content.y, static_cast<int16_t>(content.width - lane), content.height},
        strcmp(chapterTitle_, title.c_str()) == 0 ? "" : chapterTitle_, RickyDesignUi::CAPTION);
    RickyDesignUi::secondaryText(
        renderer, {static_cast<int16_t>(content.right() - lane), content.y, static_cast<int16_t>(lane), content.height},
        pages, RickyDesignUi::CAPTION, false, fui::TextAlign::Right);
  } else {
    screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), progressLine.c_str(), screen.theme().smallText);
  }
  screen.spacer(ricky::ds::space::k4);

  // menuRowItems's labels/actionValue were set once in the constructor (see
  // buildMenuRowItems()); only rows with live values need refreshing here.
  for (size_t i = 0; i < menuView.count; i++) {
    if (menuView.indices[i] == RickyReaderMenuLayout::More) continue;
    const auto action = menuItems[menuView.indices[i]].action;
    if (action == MenuAction::ROTATE_SCREEN) {
      menuRowItems[i].value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (action == MenuAction::HOLDING_HAND) {
      menuRowItems[i].value = I18N.get(holdingHandLabels[SETTINGS.pageTurnDirection != 0 ? 1 : 0]);
    } else if (action == MenuAction::AUTO_PAGE_TURN) {
      menuRowItems[i].value = pageTurnLabels[selectedPageTurnOption];
    } else if (action == MenuAction::READING_LINE) {
      menuRowItems[i].value = I18N.get(readingLineLabels[readingGuideLine::menuIndexFor(
          SETTINGS.readingGuideLineEnabled, SETTINGS.readingGuideLineStyle)]);
    } else if (action == MenuAction::READING_MODE) {
      menuRowItems[i].value =
          I18N.get(readingModeLabels[SETTINGS.readingMode < readingModeLabels.size() ? SETTINGS.readingMode : 0]);
    } else if (action == MenuAction::FONT_SIZE) {
      snprintf(fontSizeValue_, sizeof(fontSizeValue_), "%u pt", static_cast<unsigned>(SETTINGS.fontPointSize));
      menuRowItems[i].value = fontSizeValue_;
    } else if (action == MenuAction::TEXT_WEIGHT) {
      menuRowItems[i].value = I18N.get(weightLabels[SETTINGS.rickyTextWeight % weightLabels.size()]);
    } else if (action == MenuAction::NIGHT_MODE) {
      GUI.setCheckboxRow(menuRowItems[i], SETTINGS.screenInverted);
    } else if (action == MenuAction::FRONTLIGHT) {
      GUI.setCheckboxRow(menuRowItems[i], Frontlight.isOn());
    }
  }

  if (menuView.quickCount) {
    const fui::Rect band = screen.takeTop(ricky::ds::layout::kProfileCardHeight);
    constexpr int margin = ricky::ds::space::kPageMargin;
    constexpr int gap = ricky::ds::space::kGutter;
    const int width = (band.width - 2 * margin - gap * (menuView.quickCount - 1)) / menuView.quickCount;
    for (int i = 0; i < menuView.quickCount; ++i) {
      const fui::Rect cell{static_cast<int16_t>(band.x + margin + i * (width + gap)), band.y,
                           static_cast<int16_t>(width), band.height};
      const bool selected = UITheme::getInstance().showSelectionCursor() && nav.selected == i;
      RickyDesignUi::card(renderer, cell, selected);
      const auto action = menuItems[menuView.indices[i]].action;
      const freeink::Icon& icon = action == MenuAction::SELECT_CHAPTER    ? icon_ricky_contents_40
                                  : action == MenuAction::TOGGLE_BOOKMARK ? icon_ricky_bookmark_40
                                  : action == MenuAction::TEXT_SETTINGS   ? icon_ricky_fonts_40
                                                                          : icon_ricky_lookup_40;
      RickyPageUi::pageIcon(screen.target(), renderer, cell.x + (cell.width - 40) / 2, cell.y + 44, icon, selected);
      RickyDesignUi::text(renderer, {cell.x, static_cast<int16_t>(cell.y + 80), cell.width, 56}, menuRowItems[i].label,
                          RickyDesignUi::CAPTION, selected, fui::TextAlign::Center, 2);
      screen.frame().hit(cell, ACTION_ROW, static_cast<int16_t>(i), fui::InputTouch);
    }
    screen.spacer(ricky::ds::space::kBlockGap);
  }

  fui::ListProps props;
  props.items = menuRowItems + menuView.quickCount;
  props.count = menuView.count - menuView.quickCount;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = UiHighDpiProfile::enabled ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;
  props.labelText.align = fui::TextAlign::Left;
  props.toggleWidth = ricky::ds::layout::kToggleWidth;
  props.toggleHeight = ricky::ds::layout::kToggleHeight;
  props.toggleRadius = ricky::ds::layout::kToggleHeight / 2;
  props.toggleKnobRadius = ricky::ds::layout::kToggleKnobOn / 2;
  props.toggleKnobInset = (ricky::ds::layout::kToggleHeight - ricky::ds::layout::kToggleKnobOn) / 2;
  props.toggleBorderWidth = ricky::ds::stroke::kHairline;
  if (UiHighDpiProfile::enabled) {
    // Draw row accessories through FUI using the exact viewport geometry that
    // also drives touch and button paging. No second selection or hit model.
    RickyRowList::prepare(screen, props);
    screen.syncListViewport(nav, props, props.count, menuView.quickCount);
    RickyRowList::draw(
        screen, renderer, nav, props,
        [this](const int i) {
          const int original = menuView.indices[i + menuView.quickCount];
          RickyRowList::RowStyle style;
          style.icon = original == RickyReaderMenuLayout::More ? &icon_ricky_row_more_32
                                                               : &menuRowIcon(menuItems[original].action);
          style.chevron = true;
          if (original == RickyReaderMenuLayout::More) style.value = tr(STR_RICKY_MENU_MORE_SUMMARY);
          return style;
        },
        menuView.quickCount);
    return;
  }
  // The quick grid is fixed chrome, not part of the scrolling row count.
  // Touch actionValue still addresses the complete logical selection ring.
  screen.syncListViewport(nav, props, props.count, menuView.quickCount);
  screen.list(props);
}

void EpubReaderMenuActivity::drawChrome() {
  const auto& metrics = uiThemeMetrics(usesUpstreamStyle());
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight};
  GUI.drawHeaderWithStyle(renderer, header, moreView ? tr(STR_RICKY_MORE_ACTIONS) : title.c_str(), nullptr, true,
                          usesUpstreamStyle(), UiHighDpiProfile::enabled ? ricky::ds::layout::kTouchMin : 0);
  if (UiHighDpiProfile::enabled) {
    const int x = screen.x + screen.width - ricky::ds::space::kPageMargin - ricky::ds::layout::kTouchMin;
    const int y = header.y + ricky::ds::layout::kStatusBarHeight;
    auto target = makeUiTarget(renderer);
    RickyPageUi::pageIcon(target, renderer, x + (ricky::ds::layout::kTouchMin - 40) / 2,
                          y + ricky::ds::layout::kTouchMin / 2, icon_ricky_home_40);
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  drawChrome();

  renderUi();

  drawFooter();
  renderer.displayBuffer();
}
