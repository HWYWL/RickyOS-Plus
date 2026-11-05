#pragma once
#include <cstdint>
namespace plus_board {
bool begin();
enum class StandbyWake : uint8_t { Timer, PowerButton, PowerButtonHeld, Failed };
bool standbySleepReady();
// Awake-idle comparison must not slow an active playback/recording worker.
bool idleCpuFrequencyAllowed();
// Called only by the existing awake poll; diagnostics keep unplugged samples
// in the existing RAM ring, with no SD/NVS writes or new wake source.
void traceIdlePower();
// Caller owns the framebuffer lock; no rail cuts or charger/gauge writes.
StandbyWake standbyLightSleep(uint32_t seconds);
struct StandbyStorage {
  bool (*suspend)(void*);
  bool (*resume)(void*);
  void* context;
};
// Static face only: caller owns rendering and an exclusive HAL storage lease.
StandbyWake standbyStaticSleep(uint32_t seconds, StandbyStorage& storage);
}
