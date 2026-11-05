#pragma once

#include <LibraryBuilder.h>
#include <LibrarySources.h>

#include <atomic>
#include <memory>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

struct LibraryImportRequest {
  enum class Action { Refresh, AddBook, AddFolder, RemoveSource, ExcludeBook, RefreshSource };
  Action action = Action::Refresh;
  std::string path;
  bool recursive = false;
  uint16_t source = 0;
};

// The worker never renders or acquires RenderLock. Exiting requests cancellation
// and waits for storage handles to unwind before the activity can be destroyed
// or USB can acquire the card. It never force-deletes a task doing filesystem I/O.
class LibraryImportActivity final : public UiListActivity {
 public:
  LibraryImportActivity(GfxRenderer& renderer, MappedInputManager& input, LibraryImportRequest request)
      : UiListActivity("LibraryImport", renderer, input), request(std::move(request)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return !done.load(); }

 protected:
  int listCount() const override { return 4; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;

 private:
  LibraryImportRequest request;
  std::atomic<bool> cancel{false}, done{false};
  std::atomic<uint16_t> progress{0};
  bool succeeded = false;
  bool waitForButtons = false;
  uint16_t seen = 0;
  uint32_t lastPaint = 0;
  library::BuildStats stats;
  char progressText[80]{};
  static void worker(void* ctx);
  bool runImport();
};

class LibrarySourcesActivity final : public UiListActivity {
 public:
  LibrarySourcesActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("LibrarySources", renderer, input) {}
  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override { return sources ? 3 + sources->size() : 0; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&& lock) override;

 private:
  std::unique_ptr<library::LibrarySources> sources;
  OptionPopup options;
  bool loadFailed = false;
  bool waitForButtons = false;
  void reload();
  void pick(bool folder);
  void import(LibraryImportRequest request);
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
};
