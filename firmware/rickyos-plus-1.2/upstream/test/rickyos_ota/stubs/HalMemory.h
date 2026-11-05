#pragma once
#include <cstddef>
namespace HalMemory {
struct Snapshot {
  size_t freeBytes = 100000, largestBlockBytes = 100000;
};
inline Snapshot getDefaultHeap() { return {}; }
inline Snapshot getInternalHeap() { return {}; }
inline Snapshot getPsramHeap() { return {}; }
}  // namespace HalMemory
