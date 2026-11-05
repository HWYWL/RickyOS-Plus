#pragma once
#include "activities/UiListActivity.h"
#include "components/icons/rickyAvatars.h"

// The built-in animal portraits, then "import from the SD card".
class RickyAvatarPickerActivity final : public UiListActivity {
 public:
  RickyAvatarPickerActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RickyAvatarPicker", renderer, input) {}

 protected:
  int listCount() const override { return RickyAvatars::COUNT + 1; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

 private:
  static constexpr int IMPORT = RickyAvatars::COUNT;
  bool failed = false;
  bool waitForConfirmRelease = false;
};
