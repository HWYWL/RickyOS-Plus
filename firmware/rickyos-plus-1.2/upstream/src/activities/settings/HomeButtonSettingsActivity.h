#pragma once

#include "HomeButtonSettings.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class HomeButtonSettingsActivity final : public UiListActivity {
 public:
  HomeButtonSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("HomeButtonSettings", renderer, input) {}

 private:
  #if FREEINK_DEVICE_METALIO_PLUS
  static constexpr int GESTURE_COUNT = 8;  // 6 keys, touch feedback, reset
  int keyPage = -1;
  void onBackButton() override;
#else
  static constexpr int GESTURE_COUNT = 3;
#endif

  freeink::ui::ListItem rows[GESTURE_COUNT]{};
  OptionPopup optionPopup;

  int listCount() const override {
#if FREEINK_DEVICE_METALIO_PLUS
    return keyPage < 0 ? GESTURE_COUNT : keyPage == 0 ? 3 : 2;
#else
    return GESTURE_COUNT;
#endif
  }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;
};
