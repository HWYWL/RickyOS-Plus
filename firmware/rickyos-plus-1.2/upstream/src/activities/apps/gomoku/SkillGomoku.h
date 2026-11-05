#pragma once

#include <cstddef>
#include <cstdint>

// Standalone, bounded rules engine. No HAL, display, heap or traditional-game dependency.
namespace skillgomoku {
enum class Skill : uint8_t { Place, Remove, Convert, Move, Freeze, Reverse, Wipe, Rest, Resign };
struct Action {
  Skill skill = Skill::Place;
  uint8_t target = 255;
  uint8_t source = 255;
};

class Game {
 public:
  static constexpr uint8_t None = 255;
  static constexpr uint8_t MaxEnergy = 8;
  static constexpr uint16_t ActionLimit = 400;
  static constexpr size_t EncodedSize = 256;
  uint8_t size = 15;
  uint8_t turn = 1;    // 1 black, 2 white; independent of stone count
  uint8_t winner = 0;  // 0 ongoing, 1 black, 2 white, 3 draw
  uint8_t energy[2] = {3, 3};
  uint8_t cooldown[2][3] = {};
  uint8_t used[2] = {};
  uint16_t actions = 0;
  uint8_t cells[225] = {};
  Action last{};
  uint8_t lastActor = 0;
  uint8_t winStart = None;
  uint8_t winEnd = None;

  void reset(uint8_t boardSize);
  static uint8_t cost(Skill skill);
  bool available(Skill skill) const;
  bool legal(Action action) const;
  bool apply(Action action);
  uint8_t lineWinner(uint8_t* start = nullptr, uint8_t* end = nullptr) const;
  void encode(uint8_t* out) const;
  // Reject malformed records without mutating this instance.
  bool decode(const uint8_t* bytes, size_t length);
  static uint32_t checksum(const uint8_t* bytes, size_t length);
};

// Incremental one-ply tactical opponent. Same actions/energy/cooldowns as a human.
// All workspace is owned by the Activity; step() evaluates at most 4 candidates.
class Computer {
 public:
  void begin(const Game& game);
  bool step(const Game& game, Game& scratch);
  Action result() const { return best; }
  uint16_t candidateCount() const { return count; }

 private:
  Action candidates[128]{};
  uint16_t count = 0;
  uint16_t cursor = 0;
  int32_t bestScore = -2000000000;
  Action best{};
  static int32_t evaluate(const Game& game, uint8_t player);
  static int32_t pointScore(const Game& game, uint8_t point, uint8_t player);
  void add(const Game& game, Action action);
};
}  // namespace skillgomoku
