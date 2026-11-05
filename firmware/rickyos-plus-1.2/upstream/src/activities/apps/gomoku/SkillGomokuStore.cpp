#include "SkillGomokuStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
constexpr const char* Paths[] = {"/.crosspoint/skill_gomoku_v1_a.bin", "/.crosspoint/skill_gomoku_v1_b.bin"};
constexpr const char* Temporary = "/.crosspoint/skill_gomoku_v1.tmp";
uint32_t read32(const uint8_t* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (i * 8);
  return value;
}
void write32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}
}  // namespace

bool SkillGomokuStore::load(skillgomoku::Game& game, bool& versusComputer, uint8_t& cursor) {
  bool found = false;
  generation = 0;
  currentSlot = 1;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    HalFile file;
    if (!Storage.openFileForRead("SGMK", Paths[slot], file) || file.size() != sizeof(bytes) ||
        file.read(bytes, sizeof(bytes)) != sizeof(bytes))
      continue;
    const uint32_t seq = read32(bytes + 8);
    if (std::memcmp(bytes, "SGS1", 4) || bytes[4] > 1 || bytes[6] || bytes[7] || !seq ||
        read32(bytes + 268) != skillgomoku::Game::checksum(bytes, 268))
      continue;
    if (found && (seq - generation == 0 || seq - generation >= 0x80000000u)) continue;
    const uint8_t size = bytes[16];
    if ((size != 9 && size != 15) || bytes[5] >= size * size || !game.decode(bytes + 12, 256)) continue;
    generation = seq;
    currentSlot = slot;
    versusComputer = bytes[4] != 0;
    cursor = bytes[5];
    found = true;
  }
  return found;
}

bool SkillGomokuStore::save(const skillgomoku::Game& game, bool versusComputer, uint8_t cursor) {
  if (cursor >= game.size * game.size || !Storage.ensureDirectoryExists("/.crosspoint")) return false;
  std::memset(bytes, 0, sizeof(bytes));
  std::memcpy(bytes, "SGS1", 4);
  bytes[4] = versusComputer;
  bytes[5] = cursor;
  uint32_t seq = generation + 1;
  if (!seq) seq = 1;
  write32(bytes + 8, seq);
  game.encode(bytes + 12);
  write32(bytes + 268, skillgomoku::Game::checksum(bytes, 268));
  {
    HalFile file;
    if (!Storage.openFileForWrite("SGMK", Temporary, file) || file.write(bytes, sizeof(bytes)) != sizeof(bytes)) {
      LOG_ERR("SGMK", "Save write failed; previous generation retained");
      return false;
    }
    file.flush();
  }  // close before replace; currentSlot always remains untouched
  const uint8_t next = 1 - currentSlot;
  if (!Storage.replaceFile(Temporary, Paths[next])) {
    LOG_ERR("SGMK", "Save replace failed; previous generation retained");
    return false;
  }
  generation = seq;
  currentSlot = next;
  return true;
}
