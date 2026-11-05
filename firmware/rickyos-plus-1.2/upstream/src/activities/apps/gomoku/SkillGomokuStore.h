#pragma once
#include "SkillGomoku.h"

// Two independently checked generations: a failed FAT replacement must not
// destroy the only good save. Never accesses the traditional Gomoku paths.
class SkillGomokuStore {
 public:
  bool load(skillgomoku::Game& game, bool& versusComputer, uint8_t& cursor);
  bool save(const skillgomoku::Game& game, bool versusComputer, uint8_t cursor);

 private:
  uint32_t generation = 0;
  uint8_t currentSlot = 1;
  uint8_t bytes[272]{};
};
