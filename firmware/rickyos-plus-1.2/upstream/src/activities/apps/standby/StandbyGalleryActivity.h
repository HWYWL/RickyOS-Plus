#pragma once

#include <string>
#include <vector>

#include "StandbyPictures.h"
#include "activities/UiListActivity.h"

// Choose a standby, power-off, reading-background or transparent picture from
// thumbnails laid out in a grid, a page at a time; as the 图库 app, the same grid just for looking. A tile opens the
// picture full screen, where it can be set as either screen and the neighbours
// flipped through. Thumbnails are made in the background, one per loop pass, each
// shown as it lands; once the page is complete and left alone, the next page's are
// made ahead.
class StandbyGalleryActivity final : public UiListActivity {
 public:
  enum class Mode : uint8_t { Pick, Browse };
  StandbyGalleryActivity(GfxRenderer& renderer, MappedInputManager& input, StandbyPictures::Target target,
                         Mode mode = Mode::Pick)
      : UiListActivity("StandbyGallery", renderer, input), target(target), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 protected:
  int listCount() const override { return static_cast<int>(pictures.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

 private:
  enum Tool : int { FilterAll, FilterDefaults, FilterMine, PrevPage, NextPage, Download, Transfer };
  static constexpr freeink::ui::ActionId ACTION_TOOL = ACTION_USER;

  void reload();
  void runTool(int tool);
  void turnPage(int direction);
  // One missing thumbnail of the shown page, then a repaint.
  void makePendingThumbnail();
  // One thumbnail of the next page, while the shown one is complete and idle.
  void prefetchThumbnail();

  StandbyPictures::Target target;
  Mode mode;
  StandbyPictures::Filter filter = StandbyPictures::Filter::All;
  std::vector<StandbyPictures::Entry> pictures;
  std::string inUse;
  bool listFailed = false;

  // Geometry of the last page drawn (render task), read by the loop under RenderLock.
  int pageCapacity = 6;
  int thumbWidth = 0;
  int thumbHeight = 0;
  int thumbPageStart = -1;
  uint32_t thumbPending = 0;
  uint32_t thumbAttempted = 0;
  // Next-page prefetch: starts once the shown page has had nothing to make for a while.
  unsigned long pageSettledMs = 0;
  int prefetchIndex = -1;

  char pageText[24] = {};
};
