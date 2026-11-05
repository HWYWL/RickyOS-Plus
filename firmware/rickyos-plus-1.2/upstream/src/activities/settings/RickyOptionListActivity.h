#pragma once

#include <I18n.h>

#include <cstdint>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// One row of a RickyOS settings sub-page: an on/off switch (options == nullptr) or a
// choice from a list, bound to a CrossPointSettings field. A section caption is drawn
// above the row when `section` is set.
struct RickyOptionRow {
  StrId label;
  uint8_t* field;
  const StrId* options = nullptr;
  uint8_t optionCount = 0;
  StrId section = StrId::STR_NONE_OPT;
};

struct RickyOptionList {
  StrId title;
  const RickyOptionRow* rows;
  int count;
};

namespace RickyOptionLists {
// Settings -> System -> Gestures: every touch gesture in one place.
const RickyOptionList& gestures();
// Settings -> System -> Keys: the strip keys under the screen and the power key.
const RickyOptionList& keys();
}  // namespace RickyOptionLists

// A settings sub-page of switches and choices (Gestures, Keys). Each change is saved
// as soon as it is made.
class RickyOptionListActivity final : public UiListActivity {
 public:
  RickyOptionListActivity(GfxRenderer& renderer, MappedInputManager& input, const RickyOptionList& list)
      : UiListActivity("RickyOptionList", renderer, input), list(list) {}

 protected:
  int listCount() const override { return list.count; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&& lock) override;

 private:
  const RickyOptionList& list;
  OptionPopup optionPopup;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
};
