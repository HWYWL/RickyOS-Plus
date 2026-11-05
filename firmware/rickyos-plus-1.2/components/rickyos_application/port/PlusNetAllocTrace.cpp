// Observe allocation/driver return values. Copy-TX is a separate explicit experiment.
#if RICKYOS_PLUS_NET_ALLOC_TRACE
#include "PlusNetAllocTrace.h"
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_private/wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <lwip/pbuf.h>
#include <cerrno>
#include <cstdint>
#include <atomic>

namespace {
struct Counter {
  uint32_t calls = 0;
  uint32_t failed = 0;
  uint32_t noMemory = 0;
  uint32_t maxFailedBytes = 0;
  int32_t lastError = 0;
};
enum Layer : unsigned { Packet, Netif, WifiCopy, WifiRef, Count };
// Exactly 80 bytes of counters plus the port spinlock. No heap, retained packet,
// timer or task. Hook context only copies numbers under a short critical section;
// heap inspection and logging happen in the caller's normal task after unlock.
Counter counters[Count]{};
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct AllocationFailure {
  uint32_t serial;
  uint32_t bytes;
  uint32_t caps;
  uintptr_t caller;
  uint32_t kind;
};
constexpr unsigned failureCapacity = 8;
AllocationFailure allocationFailures[failureCapacity]{};
uint32_t allocationFailureCount = 0;
// Use compiler atomics directly: this build's std::atomic<bool>::load is
// emitted out of line in flash, which is unsafe in cache-disabled heap calls.
bool allocationTraceActive = false;  // Avoid RTOS operations during early heap bootstrap.
static_assert(__atomic_always_lock_free(sizeof(bool), nullptr));
#if RICKYOS_PLUS_NET_COPY_TX
std::atomic<bool> copyTx{false};
#endif
void observe(Layer layer, int32_t result, bool noMemory, size_t bytes) {
  const int savedErrno = errno;
  portENTER_CRITICAL(&mux);
  auto& value = counters[layer];
  ++value.calls;
  if (result != 0) {
    ++value.failed;
    value.noMemory += noMemory ? 1 : 0;
    value.lastError = result;
    if (bytes > value.maxFailedBytes) value.maxFailedBytes = static_cast<uint32_t>(bytes);
  }
  portEXIT_CRITICAL(&mux);
  errno = savedErrno;
}
void IRAM_ATTR allocationFailed(unsigned kind, size_t bytes, uint32_t caps, uintptr_t caller) {
  // Heap APIs may run with cache disabled or from an ISR. No logging, errno
  // accessor, timestamp function, allocator or flash-resident helper here.
  portENTER_CRITICAL_SAFE(&mux);
  const uint32_t serial = ++allocationFailureCount;
  allocationFailures[(serial - 1) % failureCapacity] = {
      serial, bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(bytes), caps, caller, kind};
  portEXIT_CRITICAL_SAFE(&mux);
}
}

#if RICKYOS_PLUS_NET_COPY_TX
void plusNetAllocTraceSetCopyTx(bool enabled) { copyTx.store(enabled, std::memory_order_relaxed); }
#endif

void plusNetAllocTraceSnapshot(const char* stage) {
  __atomic_store_n(&allocationTraceActive, true, __ATOMIC_RELAXED);
  Counter snapshot[Count];
  AllocationFailure failures[failureCapacity];
  uint32_t failCount;
  portENTER_CRITICAL(&mux);
  for (unsigned i = 0; i < Count; ++i) snapshot[i] = counters[i];
  for (unsigned i = 0; i < failureCapacity; ++i) failures[i] = allocationFailures[i];
  failCount = allocationFailureCount;
  portEXIT_CRITICAL(&mux);
  constexpr const char* labels[] = {"pbuf", "netif", "wifi-copy", "wifi-ref"};
  for (unsigned i = 0; i < Count; ++i) {
    const auto& value = snapshot[i];
    ESP_LOGI("NETALLOC", "stage=%s layer=%s calls=%lu failed=%lu nomem=%lu max_failed_bytes=%lu last=%ld",
             stage, labels[i], static_cast<unsigned long>(value.calls), static_cast<unsigned long>(value.failed),
             static_cast<unsigned long>(value.noMemory), static_cast<unsigned long>(value.maxFailedBytes),
             static_cast<long>(value.lastError));
  }
  ESP_LOGI("NETALLOC", "stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
           stage, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
  ESP_LOGI("NETALLOC", "stage=%s allocation_failures=%lu", stage, static_cast<unsigned long>(failCount));
  for (const auto& event : failures) {
    if (event.serial == 0) continue;
    ESP_LOGI("NETALLOC", "stage=%s allocation_serial=%lu kind=%lu bytes=%lu caps=0x%lx caller=0x%lx",
             stage, static_cast<unsigned long>(event.serial), static_cast<unsigned long>(event.kind),
             static_cast<unsigned long>(event.bytes), static_cast<unsigned long>(event.caps),
             static_cast<unsigned long>(event.caller));
  }
}

extern "C" {
void* __real_heap_caps_malloc(size_t, uint32_t);
__attribute__((noinline)) IRAM_ATTR void* __wrap_heap_caps_malloc(size_t bytes, uint32_t caps) {
  void* result = __real_heap_caps_malloc(bytes, caps);
  if (!result && bytes && __atomic_load_n(&allocationTraceActive, __ATOMIC_RELAXED))
    allocationFailed(0, bytes, caps, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return result;
}
void* __real_heap_caps_calloc(size_t, size_t, uint32_t);
__attribute__((noinline)) IRAM_ATTR void* __wrap_heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
  void* result = __real_heap_caps_calloc(count, size, caps);
  if (!result && count && size && __atomic_load_n(&allocationTraceActive, __ATOMIC_RELAXED))
    allocationFailed(1, count > SIZE_MAX / size ? SIZE_MAX : count * size, caps,
                     reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return result;
}
void* __real_heap_caps_realloc(void*, size_t, uint32_t);
__attribute__((noinline)) IRAM_ATTR void* __wrap_heap_caps_realloc(void* ptr, size_t bytes, uint32_t caps) {
  void* result = __real_heap_caps_realloc(ptr, bytes, caps);
  if (!result && bytes && __atomic_load_n(&allocationTraceActive, __ATOMIC_RELAXED))
    allocationFailed(2, bytes, caps, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return result;
}

pbuf* __real_pbuf_alloc(pbuf_layer, u16_t, pbuf_type);
pbuf* __wrap_pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type) {
  auto* result = __real_pbuf_alloc(layer, length, type);
  observe(Packet, result ? 0 : -1, result == nullptr, length);
  return result;
}

esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t*, void*, size_t, void*);
esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t* netif, void* data, size_t length, void* reference) {
  const auto result = __real_esp_netif_transmit_wrap(netif, data, length, reference);
  observe(Netif, result, result == ESP_ERR_NO_MEM, length);
  return result;
}

int __real_esp_wifi_internal_tx(wifi_interface_t, void*, uint16_t);
int __wrap_esp_wifi_internal_tx(wifi_interface_t interface, void* data, uint16_t length) {
  const int result = __real_esp_wifi_internal_tx(interface, data, length);
  observe(WifiCopy, result, result == ESP_ERR_NO_MEM, length);
  return result;
}

esp_err_t __real_esp_wifi_internal_tx_by_ref(wifi_interface_t, void*, size_t, void*);
esp_err_t __wrap_esp_wifi_internal_tx_by_ref(wifi_interface_t interface, void* data, size_t length, void* reference) {
#if RICKYOS_PLUS_NET_COPY_TX
  // esp_private/wifi.h documents that internal_tx takes its own copy. Do not
  // call the by-reference API first and retry: failed submissions must not
  // risk duplicate sends or ambiguous pbuf ownership. No application buffer,
  // refcount manipulation or allocation is introduced here. The driver owns
  // its copied frame. Its extra copy/memory cost remains to be measured.
  (void)reference;
  if (copyTx.load(std::memory_order_relaxed)) {
    if (length > UINT16_MAX) {
      observe(WifiRef, ESP_ERR_INVALID_ARG, false, length);
      return ESP_ERR_INVALID_ARG;
    }
    const auto result = __wrap_esp_wifi_internal_tx(interface, data, static_cast<uint16_t>(length));
    observe(WifiRef, result, result == ESP_ERR_NO_MEM, length);
    return result;
  }
#endif
  const auto result = __real_esp_wifi_internal_tx_by_ref(interface, data, length, reference);
  observe(WifiRef, result, result == ESP_ERR_NO_MEM, length);
  return result;
}
}
#endif
