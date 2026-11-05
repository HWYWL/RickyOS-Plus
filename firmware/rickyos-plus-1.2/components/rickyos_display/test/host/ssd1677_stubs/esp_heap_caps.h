#pragma once
#include <cassert>
#include <cstdlib>
inline constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
inline int allocationCalls = 0, allocationFailAt = 0, liveAllocations = 0;
inline void* heap_caps_malloc(size_t n, int caps) {
  assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (++allocationCalls == allocationFailAt) return nullptr;
  void* p = std::malloc(n);
  if (p) ++liveAllocations;
  return p;
}
inline void heap_caps_free(void* p) {
  if (p) --liveAllocations;
  std::free(p);
}
