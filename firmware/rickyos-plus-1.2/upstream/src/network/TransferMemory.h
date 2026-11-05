#pragma once

#include <BoardConfig.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>

namespace transferMemory {

#if FREEINK_DEVICE_READPICO
inline constexpr bool guarded = true;
#else
inline constexpr bool guarded = false;
#endif

// Conservative service gates, not a guarantee against every SDK allocation.
inline constexpr size_t START_FREE = 16 * 1024;
inline constexpr size_t START_BLOCK = 4 * 1024;
inline constexpr size_t RUN_FREE = 8 * 1024;
inline constexpr size_t RUN_BLOCK = 1024;

constexpr bool healthy(const HalMemory::HeapStats& heap, const bool startup) {
  return heap.freeBytes >= (startup ? START_FREE : RUN_FREE) &&
         heap.largestBlockBytes >= (startup ? START_BLOCK : RUN_BLOCK);
}

inline void logSnapshot(const char* phase) {
  if constexpr (guarded) {
    const auto internal = HalMemory::getInternalHeap();
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("NETMEM", "%s internal free/min/largest=%zu/%zu/%zu PSRAM free=%zu", phase, internal.freeBytes,
            internal.minFreeBytes, internal.largestBlockBytes, psram.freeBytes);
  }
}

inline memory::ByteBuffer allocateBuffer(const size_t bytes) {
  if (bytes == 0) return {};
#if FREEINK_DEVICE_READPICO && !defined(SIMULATOR)
  if (HalMemory::getPsramHeap().totalBytes > 0) {
    // Upload/list bytes are sequential task-context data, never DMA/ISR storage.
    // Keep the existing capacities and lifetime; only change their heap domain.
    auto buffer = HalMemory::allocatePsram(bytes);
    if (!buffer) LOG_ERR("NETMEM", "OOM: %zu-byte PSRAM transfer buffer", bytes);
    // ESP-IDF documents free() as compatible with heap_caps_malloc(). ByteBuffer
    // preserves that matched ownership also for the normal-heap fallback below.
    return memory::ByteBuffer{buffer.release()};
  }
  // No-PSRAM hardware may use the existing normal allocator only with room left
  // for service startup. Never fall back to scarce internal RAM after PSRAM OOM.
  const auto heap = HalMemory::getInternalHeap();
  if (heap.freeBytes < START_FREE || bytes > heap.freeBytes - START_FREE || heap.largestBlockBytes < bytes) {
    LOG_ERR("NETMEM", "No headroom for %zu-byte internal transfer buffer", bytes);
    return {};
  }
#endif
  memory::ByteBuffer buffer{static_cast<uint8_t*>(std::calloc(1, bytes))};
  if (!buffer) LOG_ERR("NETMEM", "OOM: %zu-byte transfer buffer", bytes);
  return buffer;
}

}  // namespace transferMemory
