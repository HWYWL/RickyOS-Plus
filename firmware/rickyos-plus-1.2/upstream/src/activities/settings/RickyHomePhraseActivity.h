#pragma once
#include <array>

#include "activities/UiListActivity.h"

class RickyHomePhraseActivity final : public UiListActivity {
 public:
  RickyHomePhraseActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RickyHomePhrase", renderer, input) {}

 protected:
  int listCount() const override { return 2; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

 private:
  std::array<freeink::ui::ListItem, 2> rows{};
  bool failed = false;
  bool waitForConfirmRelease = false;
};
