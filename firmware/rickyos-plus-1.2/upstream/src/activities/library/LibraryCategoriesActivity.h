#pragma once

#include <LibraryCategories.h>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Same screen for filtering/managing categories and for assigning one/batch of
// books. The selected IDs are sorted fingerprints, not copies of book content.
class LibraryCategoriesActivity final : public UiListActivity {
 public:
  LibraryCategoriesActivity(GfxRenderer& renderer, MappedInputManager& input,
                            std::unique_ptr<uint64_t[]> hashes = nullptr, size_t count = 0)
      : UiListActivity("LibraryCategories", renderer, input, true), hashes(std::move(hashes)), count(count) {}
  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override;
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&& lock) override;

 private:
  library::LibraryCategories categories;
  std::unique_ptr<uint64_t[]> hashes;
  size_t count;
  uint16_t any = 0, all = 0, touched = 0, enabled = 0;
  bool failed = false, waitForButtons = false;
  OptionPopup options;
  int slotForRow(int row) const;
  int leadingRows() const { return count ? 2 : 3; }
  void editName(int slot = -1);
  void showError();
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
};
