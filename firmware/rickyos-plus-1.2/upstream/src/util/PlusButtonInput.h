#pragma once
#include <cstdint>

namespace plus_keys {
// Stable persisted values. Default retains the board/navigation mapping.
enum Action : uint8_t { Default, Ignore, Back, Confirm, Previous, Next, Home, Menu, Refresh, Light, Control, Count };
constexpr unsigned KEY_COUNT = 5;
constexpr unsigned FIELD_COUNT = KEY_COUNT * 2;
constexpr uint32_t HOLD_MS = 600;
inline uint8_t valid(uint8_t value, bool hold) { return value < Count ? value : static_cast<uint8_t>(hold ? Ignore : Default); }

// Fixed storage; no queues, timers, tasks or allocation. Each contact snapshots
// its mapping and produces at most one action. A hold consumes its release.
class Gesture {
 public:
  bool blocked = false;
  void cancel() { if (previous) discarded = true; }
  uint8_t update(uint32_t now, bool down, uint8_t tap, uint8_t hold) {
    blocked = false;
    if (!ready) { ready = true; previous = down; discarded = down; }
    if (discarded) { blocked = true; if (!down) { discarded = false; previous = false; } return Count; }
    if (down && !previous) {
      started = now; shortAction = valid(tap, false); longAction = valid(hold, true);
      captured = shortAction != Default || longAction != Ignore; fired = false;
    }
    blocked = captured && (down || previous);
    uint8_t result = Count;
    if (captured && down && !fired && longAction != Ignore && now - started >= HOLD_MS) {
      fired = true; result = longAction;
    } else if (captured && !down && previous && !fired) result = shortAction;
    previous = down;
    if (!down) captured = false;
    return result;
  }
 private:
  uint32_t started = 0;
  uint8_t shortAction = Default, longAction = Ignore;
  bool ready = false, previous = false, captured = false, fired = false, discarded = false;
};
}  // namespace plus_keys
