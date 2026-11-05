#pragma once
#include <cstdint>

namespace plus_sleep_trace {
enum class Stage : uint32_t {
  Boot = 1, SdStop, SdPads, TpsSleep, BusPark, ImuPark, KeysMask, PowerInput,
  RailsOff, IoCheck, BusLatch, SleepEnter, SleepReturned, WakeLatch, WakeCleanup,
  KeysRestore, RailsOn, SdRelease, SdMount, ImuRestore, UsbRestore, LcdRestore,
  ExitTimer, ExitPower, ExitHeld, ExitFailed,
  RailP14Write, RailP14Wait, RailP14Waited, RailP5Write, RailP5Wait, RailP5Waited
};
// Two 72-byte slots, no constructors or dynamic storage: eligible for RTC_NOINIT.
// Commit the checksum last, always into the other slot, so a reset during a
// checkpoint leaves at least the preceding completed stage recoverable.
constexpr unsigned kWords = 18, kHistory = 8, kDigest = 17;
struct Record { uint32_t words[kWords]; };
struct Snapshot { Record record; bool valid; unsigned index; };
inline uint32_t digest(const Record& r) {
  uint32_t hash = 2166136261u;
  for (unsigned i = 0; i < kDigest; ++i) hash = (hash ^ r.words[i]) * 16777619u;
  return hash | 1u;  // zero is the uncommitted marker
}
inline bool valid(const Record& r) {
  return r.words[0] == 0x53504c34u && r.words[1] == 1 && r.words[5] <= kHistory &&
         r.words[kDigest] == digest(r);
}
inline Snapshot latest(const volatile Record (&slots)[2]) {
  Snapshot result{};
  for (unsigned slot = 0; slot < 2; ++slot) {
    Record candidate{};
    for (unsigned i = 0; i < kWords; ++i) candidate.words[i] = slots[slot].words[i];
    if (valid(candidate) && (!result.valid || int32_t(candidate.words[2] - result.record.words[2]) > 0))
      result = {candidate, true, slot};
  }
  return result;
}
inline void checkpoint(volatile Record (&slots)[2], Stage stage, uint32_t uptime,
                       uint32_t wakeCause = UINT32_MAX, uint32_t sleepError = 0, bool timerWake = false) {
  const auto previous = latest(slots);
  Record next = previous.record;
  next.words[0] = 0x53504c34u;
  next.words[1] = 1;
  ++next.words[2];
  if (stage == Stage::SleepEnter) ++next.words[3];
  next.words[4] = uptime;
  if (next.words[5] < kHistory) {
    next.words[6 + next.words[5]++] = static_cast<uint32_t>(stage);
  } else {
    for (unsigned i = 0; i + 1 < kHistory; ++i) next.words[6 + i] = next.words[7 + i];
    next.words[6 + kHistory - 1] = static_cast<uint32_t>(stage);
  }
  if (wakeCause != UINT32_MAX) {
    next.words[14] = wakeCause;
    next.words[15] = sleepError;
    if (timerWake && sleepError == 0) ++next.words[16];
  }
  next.words[kDigest] = digest(next);
  const unsigned slot = previous.valid ? previous.index ^ 1u : 0;
  slots[slot].words[kDigest] = 0;
  for (unsigned i = 0; i < kDigest; ++i) slots[slot].words[i] = next.words[i];
#if defined(__riscv)
  __asm__ __volatile__("fence rw,rw" ::: "memory");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
  slots[slot].words[kDigest] = next.words[kDigest];
}
}  // namespace plus_sleep_trace
