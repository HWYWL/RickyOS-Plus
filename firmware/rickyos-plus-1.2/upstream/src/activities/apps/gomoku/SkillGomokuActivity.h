#pragma once

#include "SkillGomoku.h"
#include "SkillGomokuStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

class SkillGomokuActivity final : public Activity, private UiAppHost {
 public:
  SkillGomokuActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool wantsGraySettle() const override { return true; }  // 16-level UI like the rest of RickyOS

 private:
  enum class Page : uint8_t { Menu, Board, Skills, Pause, Rules };
  skillgomoku::Game game, scratch;
  skillgomoku::Game history[8];
  skillgomoku::Computer computer;
  SkillGomokuStore store;
  uint8_t historyCount = 0;
  uint8_t cursor = 0;
  uint8_t source = skillgomoku::Game::None;
  skillgomoku::Skill armed = skillgomoku::Skill::Place;
  Page page = Page::Menu;
  Page rulesReturn = Page::Menu;
  bool versusComputer = true;
  bool hasSave = false;
  bool dirty = false;
  bool saveFailed = false;
  bool thinking = false;
  bool releaseBarrier = true;
  uint8_t selected = 0;
  uint8_t rulesPage = 0;
  const char* notice = nullptr;
  uint32_t saveDue = 0;
  // Rendered board geometry is also used to decode routed board coordinates.
  int boardX = 0, boardY = 0, pitch = 0;
  char labels[8][128]{};
  char actionNotice[128]{};

  static void screenFn(UiScreen& screen, void* user);
  static void onControl(const freeink::ui::ActionEvent& event, void* user);
  void buildScreen(UiScreen& screen);
  void drawBoard(UiScreen& screen);
  const char* pageTitle() const;
  void activate(int index);
  void back();
  void changePage(Page next);
  void start(uint8_t size, bool ai);
  void confirm();
  void commit(skillgomoku::Action action);
  void undo();
  void flushSave();
  int itemCount() const;
};
