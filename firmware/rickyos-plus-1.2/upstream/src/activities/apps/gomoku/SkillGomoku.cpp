#include "SkillGomoku.h"

#include <algorithm>
#include <cstring>

namespace skillgomoku {
namespace {
constexpr int8_t Directions[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
bool inside(int r, int c, int size) { return r >= 0 && c >= 0 && r < size && c < size; }
uint8_t cooldownIndex(Skill skill) { return static_cast<uint8_t>(skill) - 1; }
uint8_t onceBit(Skill skill) { return 1u << (static_cast<uint8_t>(skill) - 4); }
}  // namespace

void Game::reset(uint8_t boardSize) {
  *this = Game{};
  size = boardSize == 9 ? 9 : 15;
}

uint8_t Game::cost(Skill skill) {
  constexpr uint8_t Costs[] = {0, 3, 4, 2, 5, 6, 8, 0, 0};
  const auto index = static_cast<uint8_t>(skill);
  return index < sizeof(Costs) ? Costs[index] : 255;
}

bool Game::available(Skill skill) const {
  if (winner || static_cast<uint8_t>(skill) > static_cast<uint8_t>(Skill::Resign)) return false;
  if (energy[turn - 1] < cost(skill)) return false;
  if (skill >= Skill::Remove && skill <= Skill::Move && cooldown[turn - 1][cooldownIndex(skill)]) return false;
  if (skill >= Skill::Freeze && skill <= Skill::Wipe && (used[turn - 1] & onceBit(skill))) return false;
  return skill != Skill::Rest || energy[turn - 1] < MaxEnergy;
}

bool Game::legal(Action action) const {
  if (!available(action.skill)) return false;
  const int total = size * size;
  if (action.skill == Skill::Rest || action.skill == Skill::Resign) return true;
  if (action.skill == Skill::Reverse) {
    bool black = false, white = false;
    for (int i = 0; i < total; ++i) {
      black |= cells[i] == 1;
      white |= cells[i] == 2;
    }
    return black && white;
  }
  if (action.target >= total) return false;
  switch (action.skill) {
    case Skill::Place:
    case Skill::Freeze:
      return cells[action.target] == 0;
    case Skill::Remove:
    case Skill::Convert:
      return cells[action.target] == 3 - turn;
    case Skill::Move:
      return action.source < total && cells[action.source] == turn && cells[action.target] == 0;
    case Skill::Wipe: {
      const int r = action.target / size, c = action.target % size;
      for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc)
          if (inside(r + dr, c + dc, size) && cells[(r + dr) * size + c + dc]) return true;
      return false;
    }
    default:
      return false;
  }
}

uint8_t Game::lineWinner(uint8_t* start, uint8_t* end) const {
  for (int r = 0; r < size; ++r) {
    for (int c = 0; c < size; ++c) {
      const uint8_t player = cells[r * size + c];
      if (!player) continue;
      for (const auto& d : Directions) {
        const int er = r + 4 * d[0], ec = c + 4 * d[1];
        if (!inside(er, ec, size)) continue;
        bool five = true;
        for (int k = 1; k < 5; ++k) five &= cells[(r + k * d[0]) * size + c + k * d[1]] == player;
        if (five) {
          if (start) *start = r * size + c;
          if (end) *end = er * size + ec;
          return player;
        }
      }
    }
  }
  return 0;
}

bool Game::apply(Action action) {
  if (!legal(action)) return false;
  const uint8_t actor = turn;
  energy[actor - 1] -= cost(action.skill);
  for (auto& cd : cooldown[actor - 1])
    if (cd) --cd;
  if (action.skill >= Skill::Remove && action.skill <= Skill::Move)
    cooldown[actor - 1][cooldownIndex(action.skill)] = 2;
  if (action.skill >= Skill::Freeze && action.skill <= Skill::Wipe) used[actor - 1] |= onceBit(action.skill);
  switch (action.skill) {
    case Skill::Place:
      energy[actor - 1] = std::min<int>(MaxEnergy, energy[actor - 1] + 1);
      cells[action.target] = actor;
      break;
    case Skill::Freeze:
    case Skill::Convert:
      cells[action.target] = actor;
      break;
    case Skill::Remove:
      cells[action.target] = 0;
      break;
    case Skill::Move:
      cells[action.source] = 0;
      cells[action.target] = actor;
      break;
    case Skill::Reverse:
      for (int i = 0; i < size * size; ++i)
        if (cells[i]) cells[i] = 3 - cells[i];
      break;
    case Skill::Wipe: {
      const int r = action.target / size, c = action.target % size;
      for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc)
          if (inside(r + dr, c + dc, size)) cells[(r + dr) * size + c + dc] = 0;
      break;
    }
    case Skill::Rest:
      energy[actor - 1] = std::min<int>(MaxEnergy, energy[actor - 1] + 3);
      break;
    case Skill::Resign:
      winner = 3 - actor;
      break;
  }
  ++actions;
  last = action;
  lastActor = actor;
  if (!winner) winner = lineWinner(&winStart, &winEnd);
  if (!winner) {
    bool full = true;
    for (int i = 0; i < size * size; ++i) full &= cells[i] != 0;
    if (full || actions >= ActionLimit) winner = 3;
  }
  if (action.skill != Skill::Freeze) turn = 3 - actor;
  return true;
}

uint32_t Game::checksum(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void Game::encode(uint8_t* out) const {
  std::memset(out, 0, EncodedSize);
  std::memcpy(out, "SGM1", 4);
  out[4] = size;
  out[5] = turn;
  out[6] = winner;
  out[7] = winStart;
  out[8] = winEnd;
  std::memcpy(out + 9, energy, 2);
  std::memcpy(out + 11, cooldown, 6);
  std::memcpy(out + 17, used, 2);
  out[19] = actions & 255;
  out[20] = actions >> 8;
  out[21] = static_cast<uint8_t>(last.skill);
  out[22] = last.target;
  out[23] = last.source;
  out[24] = lastActor;
  std::memcpy(out + 25, cells, 225);
  const uint32_t crc = checksum(out, 252);
  for (int i = 0; i < 4; ++i) out[252 + i] = crc >> (i * 8);
}

bool Game::decode(const uint8_t* b, size_t length) {
  if (!b || length != EncodedSize || std::memcmp(b, "SGM1", 4)) return false;
  uint32_t crc = 0;
  for (int i = 0; i < 4; ++i) crc |= uint32_t(b[252 + i]) << (i * 8);
  if (crc != checksum(b, 252) || b[250] || b[251]) return false;
  const int n = b[4];
  const uint16_t actionCount = b[19] | (uint16_t(b[20]) << 8);
  if ((n != 9 && n != 15) || b[5] < 1 || b[5] > 2 || b[6] > 3 || actionCount > ActionLimit ||
      b[21] > static_cast<uint8_t>(Skill::Resign) || b[24] > 2 || b[9] > MaxEnergy || b[10] > MaxEnergy || b[17] > 7 ||
      b[18] > 7)
    return false;
  for (int i = 11; i < 17; ++i)
    if (b[i] > 2) return false;
  for (int offset : {7, 8, 22, 23})
    if (b[offset] != None && b[offset] >= n * n) return false;
  int stones = 0;
  for (int i = 0; i < 225; ++i) {
    if (b[25 + i] > 2 || (i >= n * n && b[25 + i])) return false;
    stones += b[25 + i] != 0;
  }
  if (stones > actionCount || (actionCount == 0) != (b[24] == 0)) return false;
  if (actionCount && b[5] != (b[21] == static_cast<uint8_t>(Skill::Freeze) ? b[24] : 3 - b[24])) return false;
  // Check terminal consistency directly against bytes before publishing anything.
  uint8_t wins = 0;
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c)
      for (const auto& d : Directions) {
        if (!b[25 + r * n + c] || !inside(r + 4 * d[0], c + 4 * d[1], n)) continue;
        bool five = true;
        for (int k = 1; k < 5; ++k) five &= b[25 + (r + k * d[0]) * n + c + k * d[1]] == b[25 + r * n + c];
        if (five) wins |= 1u << (b[25 + r * n + c] - 1);
      }
  if (wins == 3 || (!b[6] && (wins || stones == n * n || actionCount == ActionLimit))) return false;
  if (b[6] == 3 && (wins || (stones != n * n && actionCount != ActionLimit))) return false;
  if (b[6] == 1 || b[6] == 2) {
    if (b[21] == static_cast<uint8_t>(Skill::Resign)) {
      if (b[6] != 3 - b[24] || wins) return false;
    } else if (wins != (1u << (b[6] - 1)))
      return false;
  }
  if (!wins) {
    if (b[7] != None || b[8] != None) return false;
  } else {
    if (b[7] == None || b[8] == None) return false;
    const int dr = int(b[8] / n) - b[7] / n, dc = int(b[8] % n) - b[7] % n;
    if (!((dr == 0 && (dc == 4 || dc == -4)) || (dr == 4 && (dc == 0 || dc == 4 || dc == -4)))) return false;
    for (int k = 0; k < 5; ++k)
      if (b[25 + (b[7] / n + k * dr / 4) * n + b[7] % n + k * dc / 4] != b[6]) return false;
  }
  size = n;
  turn = b[5];
  winner = b[6];
  winStart = b[7];
  winEnd = b[8];
  std::memcpy(energy, b + 9, 2);
  std::memcpy(cooldown, b + 11, 6);
  std::memcpy(used, b + 17, 2);
  actions = actionCount;
  last = {static_cast<Skill>(b[21]), b[22], b[23]};
  lastActor = b[24];
  std::memcpy(cells, b + 25, 225);
  return true;
}

int32_t Computer::pointScore(const Game& game, uint8_t point, uint8_t player) {
  const int r = point / game.size, c = point % game.size;
  int32_t score = 0;
  constexpr int Weights[] = {0, 3, 24, 240, 24000, 200000};
  for (const auto& d : Directions) {
    int length = 1, open = 0;
    for (int sign : {-1, 1}) {
      int rr = r + sign * d[0], cc = c + sign * d[1];
      while (inside(rr, cc, game.size) && game.cells[rr * game.size + cc] == player) {
        ++length;
        rr += sign * d[0];
        cc += sign * d[1];
      }
      if (inside(rr, cc, game.size) && !game.cells[rr * game.size + cc]) ++open;
    }
    score += Weights[std::min(5, length)] * (open + 1);
  }
  return score;
}

int32_t Computer::evaluate(const Game& game, uint8_t player) {
  if (game.winner) return game.winner == player ? 100000000 : game.winner == 3 ? 0 : -100000000;
  constexpr int Weights[] = {0, 1, 12, 160, 8000, 1000000};
  int32_t score = 0;
  for (int r = 0; r < game.size; ++r)
    for (int c = 0; c < game.size; ++c)
      for (const auto& d : Directions) {
        if (!inside(r + 4 * d[0], c + 4 * d[1], game.size)) continue;
        int own = 0, opp = 0;
        for (int k = 0; k < 5; ++k) {
          const auto stone = game.cells[(r + k * d[0]) * game.size + c + k * d[1]];
          own += stone == player;
          opp += stone == 3 - player;
        }
        if (!opp) score += Weights[own];
        if (!own) score -= Weights[opp] * 2;
      }
  // A skipped opponent turn is a real advantage, but not an instant win.
  if (game.turn == player) score += 700;
  return score + 8 * (int(game.energy[player - 1]) - game.energy[2 - player]);
}

void Computer::add(const Game& game, Action action) {
  if (count < 128 && game.legal(action)) candidates[count++] = action;
}

void Computer::begin(const Game& game) {
  count = cursor = 0;
  bestScore = -2000000000;
  best = {};
  if (game.winner) return;
  // Tiny bounded ranked lists, not a tree search or heap-owned move graph.
  uint8_t empty[8], own[8], opponent[8];
  int32_t rank[3][8];
  for (int group = 0; group < 3; ++group)
    for (int i = 0; i < 8; ++i) rank[group][i] = -2000000000;
  std::memset(empty, Game::None, sizeof(empty));
  std::memset(own, Game::None, sizeof(own));
  std::memset(opponent, Game::None, sizeof(opponent));
  uint8_t* groups[] = {empty, own, opponent};
  for (int i = 0; i < game.size * game.size; ++i) {
    const int group = game.cells[i] == 0 ? 0 : game.cells[i] == game.turn ? 1 : 2;
    int32_t score = pointScore(game, i, game.turn) + pointScore(game, i, 3 - game.turn) * 2;
    if (!game.actions)
      score = 1000 - (i / game.size - game.size / 2) * (i / game.size - game.size / 2) -
              (i % game.size - game.size / 2) * (i % game.size - game.size / 2);
    for (int j = 0; j < 8; ++j) {
      if (score <= rank[group][j]) continue;
      for (int k = 7; k > j; --k) {
        rank[group][k] = rank[group][k - 1];
        groups[group][k] = groups[group][k - 1];
      }
      rank[group][j] = score;
      groups[group][j] = i;
      break;
    }
  }
  for (int i = 0; i < 8; ++i) {
    add(game, {Skill::Place, empty[i]});
    add(game, {Skill::Remove, opponent[i]});
    add(game, {Skill::Convert, opponent[i]});
    add(game, {Skill::Freeze, empty[i]});
    add(game, {Skill::Wipe, opponent[i]});
    for (int j = 0; j < 8; ++j) add(game, {Skill::Move, empty[i], own[j]});
  }
  add(game, {Skill::Reverse});
  if (game.energy[game.turn - 1] <= 2 && game.actions >= 4) add(game, {Skill::Rest});
}

bool Computer::step(const Game& game, Game& scratch) {
  for (int batch = 0; batch < 4 && cursor < count; ++batch) {
    const auto candidate = candidates[cursor++];
    scratch = game;
    if (!scratch.apply(candidate)) continue;
    const int32_t score = evaluate(scratch, game.turn);
    if (score > bestScore) {
      bestScore = score;
      best = candidate;
    }
  }
  return cursor >= count;
}
}  // namespace skillgomoku
