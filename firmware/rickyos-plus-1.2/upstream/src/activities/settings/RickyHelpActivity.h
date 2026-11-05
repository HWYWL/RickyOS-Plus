#pragma once

#include "activities/UiListActivity.h"

// Flash-backed help copy; no network request or full-screen bitmap allocation.
class RickyHelpActivity final : public UiListActivity {
 public:
  RickyHelpActivity(GfxRenderer& renderer, MappedInputManager& input) : UiListActivity("RickyHelp", renderer, input) {}

 protected:
  int listCount() const override { return detail_ < 0 ? 5 : 3; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;

 private:
  int detail_ = -1;
  freeink::ui::ListItem rows_[5]{};
};
