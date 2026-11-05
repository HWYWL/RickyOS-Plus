#pragma once

#include <cstdint>

#include "RecentBooksStore.h"
#include "Rect.h"
#include "UiAppHost.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class RickyHomeUi final : public UiAppHost {
 public:
  using CoverPainter = bool (*)(void*, int, const Rect&);
  explicit RickyHomeUi(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}
  void begin(const std::vector<RecentBook>& recent, CoverPainter painter, void* owner);
  void configure(Rect content, int selection, bool showSelection, int progress);
  int selectedAction(const MappedInputManager& input);
  int coverHeight() const { return thumbnailHeight; }
  static constexpr int LIBRARY = -3;
  static constexpr int STATISTICS = -4;
  static constexpr int PROFILE = -5;
  static constexpr int TRANSFER = -6;
  static constexpr int WEREAD = -7;
  static constexpr int FILES = -8;

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  bool paint(freeink::ui::Rect rect, int index);
  void draw(UiScreen& screen);
  void drawPortrait(UiScreen& screen);
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  CoverPainter painter = nullptr;
  void* owner = nullptr;
  Rect content{};
  int selected = 0;
  int start = 0;
  int progress = 0;
  int pending = -1;
  int thumbnailHeight = 0;
  bool showSelection = false;
  uint8_t greetingChoice = 0;
  char progressText[8]{};
  char dateText[32]{};
  char greetingText[128]{};
  char statsText[128]{};
  // Host and props are activity-owned, never on the small render-task stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
};
