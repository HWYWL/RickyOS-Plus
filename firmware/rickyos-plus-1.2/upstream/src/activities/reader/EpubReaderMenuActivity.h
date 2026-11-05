#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "components/RickyCompanion.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "components/RickyReaderMenuLayout.h"

class EpubReaderMenuActivity final : public UiListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DICTIONARY,
    IMAGE_SCALING,  // Legacy action ID; image scaling remains in Settings.
    READING_MODE,   // 原版 / 小狗伴读 / 沉浸 / 古籍; applied when the menu closes.
    READING_LINE,   // 阅读线: off or one of its looks; redrawn when the menu closes.
    FONT_SIZE,      // 字号: re-paginated when the menu closes.
    TEXT_WEIGHT,    // 字重: redrawn when the menu closes.
    HOLDING_HAND    // 惯用手 (pageTurnDirection): mirrors the page-turn taps and swipes at once.
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static void buildMenuItems(std::vector<MenuItem>& items, bool hasFootnotes, bool hasBookmarks);

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, bool hasBookmarks,
                                  const char* chapterTitle = "", bool pageBookmarked = false,
                                  unsigned bookmarkCount = 0);

  void render(RenderLock&&) override;
  void onEnter() override;
  bool handleHomeGesture() override;

 private:
  // Row storage: menuItems is at most MAX_MENU_ITEMS, so a
  // fixed-capacity array avoids any heap allocation for the row list. Labels
  // are set once in the constructor (buildMenuRowItems()); buildScreen()
  // only refreshes rows whose values reflect live state.
  static constexpr size_t MAX_MENU_ITEMS = RickyReaderMenuLayout::kMaxItems;
  freeink::ui::ListItem menuRowItems[MAX_MENU_ITEMS]{};
  void buildMenuRowItems();

  int listCount() const override { return menuView.count; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input runs before any button or touch handling.
  bool handleCustomInput() override;
  // Back closes on RELEASE and Confirm activates on RELEASE; everything else
  // (row navigation, page jumps) falls through to the base handler.
  bool handleButtons() override;
  // Header via GUI.drawHeader inside the safe area for the battery indicator.
  void drawChrome() override;

  void closeCancelled();
  void finishWithAction(MenuAction action);
  void setMenuView(bool more);
  RickyReaderMenuLayout::View menuView;
  bool moreView = false;
  char progressValue[8]{};
  char chapterTitle_[192]{};  // bounded snapshot, no extra dynamic string
  char bookmarkCount_[24]{};
  bool pageBookmarked_ = false;

  // Fixed menu layout
  std::vector<MenuItem> menuItems;

  OptionPopup optionPopup{true};
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> holdingHandLabels = {StrId::STR_PAGE_TURN_LTR, StrId::STR_PAGE_TURN_RTL};
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  const std::vector<StrId> readingModeLabels = {StrId::STR_READING_MODE_ORIGINAL,
                                                RickyCompanion::pick(StrId::STR_READING_MODE_PUPPY,
                                                                     StrId::STR_READING_MODE_KITTY),
                                                StrId::STR_READING_MODE_IMMERSIVE, StrId::STR_READING_MODE_ANCIENT};
  // readingGuideLine::kMenuStyles order, with 关 first.
  const std::vector<StrId> readingLineLabels = {StrId::STR_STATE_OFF,     StrId::STR_SOLID_LINE, StrId::STR_DASH_LINE,
                                                StrId::STR_DOTTED_LINE,   StrId::STR_WAVY_LINE,  StrId::STR_DOUBLE_LINE,
                                                StrId::STR_HIGHLIGHT_LINE};
  // Body-text weight steps (readerTextWeightCurve); the middle one is the font as drawn.
  const std::vector<StrId> weightLabels = {StrId::STR_RICKY_WEIGHT_THINNEST, StrId::STR_RICKY_WEIGHT_THIN,
                                           StrId::STR_FAKE_BOLD_STANDARD, StrId::STR_RICKY_WEIGHT_BOLD,
                                           StrId::STR_RICKY_WEIGHT_BOLDEST};
  // The point sizes the active family ships, filled when 字号 is opened.
  std::vector<uint8_t> fontSizes_;
  std::vector<std::string> fontSizeLabels_;
  char fontSizeValue_[8] = "";
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
