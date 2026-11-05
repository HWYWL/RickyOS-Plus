#pragma once

#include "activities/UiListActivity.h"

// RickyOS badges (util/RickyBadges.h): every badge as a medallion, earned ones solid with
// the day they were earned, the rest dotted with what they ask for and how far along.
class RickyBadgesActivity final : public UiListActivity {
 public:
  RickyBadgesActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RickyBadges", renderer, input) {}

 protected:
  int listCount() const override { return 0; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
};
