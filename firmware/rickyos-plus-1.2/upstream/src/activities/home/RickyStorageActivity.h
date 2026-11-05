#pragma once
#include <array>

#include "activities/UiListActivity.h"

class RickyStorageActivity final : public UiListActivity {
 public:
  RickyStorageActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RickyStorage", renderer, input) {}
  MainTab mainTab() const override { return MainTab::StorageFiles; }
  void selectMainTabContentEdge(MainTabContentEdge edge) override {
    nav.requestSelection(MainTabs::contentEdgeIndex(edge, kCardCount));  // Refresh is never an edge
  }

 protected:
  // The SD summary is display-only. All files, Library folders and Transfer
  // (0..2) are followed by the header's Refresh.
  static constexpr int kCardCount = 3;
  static constexpr int kRefreshAction = kCardCount;
  int listCount() const override { return kCardCount + 1; }
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawChrome() override;
  void drawFooter() override;

 private:
  enum class Space : uint8_t { NotMeasured, Measuring, Ready, Unavailable };
  // Folder behind a content card; the first existing candidate wins.
  static const char* folderFor(int index);
  bool folderMissing = false;
  Space space = Space::NotMeasured;
  uint64_t sdTotalBytes = 0;
  uint64_t sdFreeBytes = 0;
  std::array<int, 4> counts{-1, -1, -1, -1};  // books, fonts, images, downloads; -1 = unknown
  // What the used space is made of (from the last Refresh); sized = false until measured.
  bool sized = false;
  uint64_t bookBytes = 0, cacheBytes = 0, fontBytes = 0;
  int libraryFolders = -1;  // enrolled Library folders; -1 = unknown
  void countLibraryFolders();
  uint32_t seenScans = 0;                     // background results already applied to this page
  bool counting = false;                      // a background pass is running

 public:
  // Something changed the card: saved counts are stale; Refresh recomputes them.
  static void invalidateScan();

 private:
  void applyLastScan();
  void refresh();
};
