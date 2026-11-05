#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Picks the folder the Library shelf shows, one level at a time: a folder with
// subfolders opens, Back goes up, and the first row takes the folder being viewed
// (with everything under it). Counts include subfolders. The result is
// FilePathResult{path}, "" for every folder. The caller builds the folder list from
// the open index, so this screen never touches the card.
class LibraryFoldersActivity final : public UiListActivity {
 public:
  struct Folder {
    std::string path;  // as indexed, e.g. "/books/金庸"
    uint16_t count = 0;
  };
  LibraryFoldersActivity(GfxRenderer& renderer, MappedInputManager& input, std::vector<Folder> folders,
                         std::string selected)
      : UiListActivity("LibraryFolders", renderer, input, true),
        folders(std::move(folders)),
        selected(std::move(selected)) {}
  void onEnter() override;

 protected:
  int listCount() const override { return static_cast<int>(rows.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;

 private:
  struct Row {
    std::string label;
    std::string path;
    std::string value;
    uint32_t count = 0;
    bool opens = false;  // has subfolders: activating goes one level down
  };
  std::vector<Folder> folders;
  std::string selected;  // the shelf's current folder, "" for all
  std::string root;      // the top level shown (single-child levels skipped)
  std::string level;     // the folder being viewed
  std::string title;
  std::vector<Row> rows;
  bool waitForButtons = false;
  void show(const std::string& path);
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
};
