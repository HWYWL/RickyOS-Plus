#pragma once
#include <cstddef>
#include <cstdint>
namespace HalRecorder {
enum class State : uint8_t { Idle, Recording, Playing, Saved, Error };
struct Entry {
  char name[40]{};
  uint32_t seconds = 0;
};
bool startRecording();
bool play(const char* name, uint8_t volume);
void stop();
void poll();
State state();
bool active();
// True until the first PCM block has been saved; independent of whole-second timing.
bool preparing();
uint32_t seconds();
// Fixed-capacity, paged enumeration; no whole-directory vector.
size_t list(size_t offset, Entry* entries, size_t capacity, bool& more);
}  // namespace HalRecorder
