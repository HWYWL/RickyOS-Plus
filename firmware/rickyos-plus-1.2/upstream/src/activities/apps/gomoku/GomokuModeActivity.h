#pragma once

#include "activities/UiListActivity.h"

// Entry screen only. The traditional menu/game/AI/store are not modified.
class GomokuModeActivity final : public UiListActivity {
 public:
  GomokuModeActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;

 private:
  freeink::ui::ListItem rows[2]{};
  bool releaseBarrier = true;
  int listCount() const override { return 2; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;
};
