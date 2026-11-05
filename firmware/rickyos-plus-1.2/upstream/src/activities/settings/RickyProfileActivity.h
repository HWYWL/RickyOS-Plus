#pragma once
#include "activities/UiListActivity.h"

class RickyProfileActivity final : public UiListActivity {
 public:
  RickyProfileActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RickyProfile", renderer, input) {}

 public:
  void onEnter() override;

 protected:
  int listCount() const override { return 5; }  // nickname, avatar, reset avatar, home note, theme
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

 private:
  static constexpr freeink::ui::ActionId ACTION_THEME = ACTION_USER;  // value: companion to pick
  static void onThemeEvent(const freeink::ui::ActionEvent& event, void* user);
  void setCompanion(uint8_t companion);
  bool waitForConfirmRelease = false;
  bool failed = false;
};
