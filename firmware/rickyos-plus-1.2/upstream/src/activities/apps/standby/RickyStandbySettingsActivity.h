#pragma once

#include <string>
#include <vector>

#include "StandbyPictures.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// 壁纸: what the screen shows in Standby, once powered off and under the reading page,
// as one page with a live preview. Standby tab: the preview with the corner, fit and
// filter choices beside it, the five styles as cards, the picture strip (or the
// transparent overlay's controls), and when Standby starts. Power-off tab: the
// preview, the four power-off screens as cards, and when Standby powers off by
// itself. Reading tab: a sample page on the background, the paper (a picture or ruled
// lines, grid or dots), its shade, and the picture.
// Every control is one stop on the tab ring, in reading order.
class RickyStandbySettingsActivity final : public UiTabListActivity {
 public:
  RickyStandbySettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiTabListActivity("RickyStandbySettings", renderer, input) {}

  enum Tab : int { StandbyTab, PowerOffTab, ReadingTab, TabCount };
  // One focusable control; the active tab lists the ones it shows, in ring order.
  enum Item : uint8_t {
    Info0,
    Info1,
    Info2,
    Fit0,
    Fit1,
    Filter0,
    Filter1,
    Filter2,
    Layout0,
    Layout1,
    Layout2,
    Excerpt0,
    Excerpt1,
    StandbyNow,
    Style0,
    Style1,
    Style2,
    Style3,
    Style4,
    Picture0,
    Picture1,
    Picture2,
    Picture3,
    AllPictures,
    ChooseSticker,
    ClearSticker,
    AutoStandby,
    PowerOff0,
    PowerOff1,
    PowerOff2,
    PowerOff3,
    ChangePowerOffPicture,
    AutoPowerOff,
    ReadingPaper0,
    ReadingPaper1,
    ReadingPaper2,
    ReadingPaper3,
    ReadingShade0,
    ReadingShade1,
    ReadingShade2,
    ReadingShade3,
    ChangeReadingPicture,
    ItemKinds
  };
  static constexpr int kStripTiles = 4;

 protected:
  void onEnter() override;
  void onExit() override;
  void loop() override;
  int listCount() const override { return itemCount_; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void render(RenderLock&& lock) override;

  int tabCount() const override { return TabCount; }
  int activeTab() const override { return tab_; }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;

 private:
  // The controls the active tab shows now; the Standby tab's depend on its style.
  void rebuildRows();
  void reloadPictures();
  void drawPreview(const Rect& box) const;
  void drawReadingPreview(const freeink::ui::Rect& box) const;
  void openGallery(StandbyPictures::Target target);
  void makePendingThumbnail();

  OptionPopup optionPopup;
  int tab_ = StandbyTab;
  Item items_[ItemKinds] = {};
  int itemCount_ = 0;
  char minutes_[24] = {};  // the auto-standby value when it is not one of the presets
  char allPictures_[24] = {};

  // The picture strip: the gallery's first pictures, thumbnails made in the background.
  std::vector<StandbyPictures::Entry> strip_;
  int pictureCount_ = 0;
  std::string standbySource_;
  int thumbWidth_ = 0;
  int thumbHeight_ = 0;
  uint32_t thumbPending_ = 0;
  uint32_t thumbAttempted_ = 0;
};
