#pragma once

#include <SdCardFontRegistry.h>

#include <array>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Local-first, no network on entry. The fixed row store lives in the fallible
// activity allocation (PSRAM-eligible on Pico), not the render-task stack.
// Labels borrow the one existing registry; there is no duplicate font catalog.
class FontLibraryActivity final : public UiListActivity {
 public:
  FontLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int ROOT_ACTIONS = 2;  // download, import guide; Text settings lives in Reading
  std::array<freeink::ui::ListItem, SdCardFontRegistry::MAX_SD_FAMILIES + ROOT_ACTIONS> rows_{};
  int rowCount_ = 0;
  int familyIndex_ = -1;
  freeink::ui::ListNav rootNav_;
  OptionPopup popup_;

  int listCount() const override { return rowCount_; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen&) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;
  void rebuildRows();
  void refreshAfterChild();
  void showInfo(StrId message);
  void openTextSettings(bool useSelectedFamily);
  void confirmDelete();
};
