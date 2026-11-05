#pragma once
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalRecorder.h>

#include <array>

#include "activities/UiListActivity.h"
class RecorderActivity final : public UiListActivity {
 public:
  RecorderActivity(GfxRenderer& renderer, MappedInputManager& input) : UiListActivity("Recorder", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override;
  bool preventAutoSleep() override { return HalRecorder::active(); }

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawChrome() override;

 private:
  void reload();
  static constexpr size_t pageSize = 5;
  std::array<HalRecorder::Entry, pageSize> files{};
  std::array<freeink::ui::ListItem, pageSize + 3> rows{};
  std::array<std::array<char, 16>, pageSize> durations{};
  size_t offset = 0, count = 0;
  bool more = false, waitRelease = false;
  HalRecorder::State shownState = HalRecorder::State::Idle;
  uint32_t shownSeconds = 0;
  bool shownPreparing = false;
  char status[160]{};
  const char* statusLabel() const;
};
#endif
