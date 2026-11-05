#include "NetworkStartup.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include <BoardConfig.h>

#if !defined(SIMULATOR) && FREEINK_CAP_BLE_HID_HOST
#include <esp_bt.h>  // boards without Bluetooth (the Plus) have no BT component
#endif
#if !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#endif

#include "BleInput.h"
#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"

namespace {

constexpr size_t MIN_FREE_PSRAM = 256 * 1024;
constexpr size_t MIN_FREE_INTERNAL = 64 * 1024;
constexpr size_t MIN_LARGEST_INTERNAL_BLOCK = 32 * 1024;

struct MemorySnapshot {
  size_t freePsram;
  size_t freeInternal;
  size_t largestInternalBlock;
};

constexpr bool shouldReleaseRenderMemory(const MemorySnapshot& memory) {
  return memory.freePsram < MIN_FREE_PSRAM || memory.freeInternal < MIN_FREE_INTERNAL ||
         memory.largestInternalBlock < MIN_LARGEST_INTERNAL_BLOCK;
}

static_assert(shouldReleaseRenderMemory({0, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM - 1, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL - 1, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK - 1}));
static_assert(!shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));

MemorySnapshot readMemorySnapshot() {
#if defined(SIMULATOR)
  return {MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK};
#elif defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_USE_MALLOC
  constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(INTERNAL_CAPS),
          heap_caps_get_largest_free_block(INTERNAL_CAPS)};
#else
  return {};
#endif
}

}  // namespace

namespace NetworkStartup {

namespace {
bool btReleased = false;

// The BLE controller reserves internal RAM at boot whether or not a page turner is used.
// With Bluetooth off, the Wi-Fi driver plus the transfer web server do not fit beside the
// panel driver in what is left (hotspot transfer refused for low memory on Read Pico 1.2),
// so hand that memory to the heap before Wi-Fi starts. One-way until the next restart.
void releaseIdleBluetooth() {
#if !defined(SIMULATOR) && FREEINK_CAP_BLE_HID_HOST
  if (btReleased || SETTINGS.bluetoothEnabled) return;
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) return;
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  btReleased = err == ESP_OK;
  LOG_INF("NET", "BLE controller memory %s: internal free %u -> %u", btReleased ? "released" : "kept",
          static_cast<unsigned>(before),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
}
}  // namespace

bool bluetoothMemoryReleased() { return btReleased; }

void logMemory(const char* stage) {
  const auto internal = HalMemory::getInternalHeap();
#if defined(SIMULATOR)
  const HalMemory::HeapStats dma{};  // No hardware DMA heap in the desktop simulator.
#else
  const auto dma = HalMemory::getInternalDmaHeap();
#endif
  const auto psram = HalMemory::getPsramHeap();
  LOG_INF("NET", "%s: internal free=%u largest=%u, DMA free=%u largest=%u, PSRAM free=%u", stage,
          static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.largestBlockBytes),
          static_cast<unsigned>(dma.freeBytes), static_cast<unsigned>(dma.largestBlockBytes),
          static_cast<unsigned>(psram.freeBytes));
}

void prepare(GfxRenderer& renderer) {
  logMemory("before font reclaim");
  const MemorySnapshot before = readMemorySnapshot();
  if (!shouldReleaseRenderMemory(before)) return;

  {
    RenderLock lock;
    sdFontSystem.releaseLoadedFont(renderer);
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
  }
  logMemory("after font reclaim");
}

void recordFailure(const char* stage, const int code) {
  constexpr const char* PATH = "/.crosspoint/wifi-errors.txt";
  constexpr size_t MAX_BYTES = 4096;
  const auto internal = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  char line[160];
  const int len = snprintf(line, sizeof(line), "%s up=%lus %s code=%d internal=%u/%u psram=%u\n", CROSSPOINT_VERSION,
                           static_cast<unsigned long>(millis() / 1000), stage, code,
                           static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.largestBlockBytes),
                           static_cast<unsigned>(psram.freeBytes));
  if (len <= 0) return;

  RenderLock lock;
  Storage.ensureDirectoryExists("/.crosspoint");
  oflag_t flags = O_WRONLY | O_CREAT | O_APPEND;
  {
    HalFile existing = Storage.open(PATH);
    if (existing && existing.fileSize() > MAX_BYTES) flags = O_WRONLY | O_CREAT | O_TRUNC;
  }
  HalFile file = Storage.open(PATH, flags);
  if (!file) return;
  file.write(line, std::min(static_cast<size_t>(len), sizeof(line) - 1));
  file.close();
}

bool setMode(GfxRenderer& renderer, const wifi_mode_t mode) {
  bleinput::stop();
  releaseIdleBluetooth();
  logMemory("after BLE stop");
  prepare(renderer);
  const bool started = WiFi.mode(mode);
  logMemory(started ? "WiFi mode ready" : "WiFi mode failed");
  return started;
}

}  // namespace NetworkStartup
