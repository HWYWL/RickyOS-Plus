// Opt-in field recording: identical observer on cable and battery power.
#if RICKYOS_PLUS_FIELD_DIAG
#include "PlusFieldDiagnostics.h"
#include <Arduino.h>
#include <BatteryMonitor.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <epd_board_metalio_eink4_plus.h>
#include <lcd_fault.h>
#include <metalio_i2c1.h>
#include <plus_i2c_wire.h>
#include "activities/RenderLock.h"
#include <cstdio>

extern "C" void epd_lcd_get_timing(int*, int*, int*);

namespace {
constexpr char kPath[] = "/.crosspoint/plus-display-memory-v1.csv";
constexpr size_t kLimit = 128 * 1024;
constexpr uint32_t kInterval = 5000;
constexpr char kHeader[] =
    "up_ms,mv,mv_ok,current_ma,current_ok,average_ma,average_ok,sample_cpu_mhz,charger_1e,"
    "last_tps_start_ok,first_lcd_fault,phase_avg_us,phase_min_us,phase_max_us,"
    "internal_free,internal_min,internal_largest,dma_free,dma_min,dma_largest,"
    "psram_free,psram_min,psram_largest,previous_write_ms\n";
uint32_t lastAt = 0, previousWriteMs = 0;
bool stopped = false, sessionHeaderWritten = false;
}

void plusFieldDiagnosticsPoll() {
  const uint32_t now = millis();
  if (stopped || uint32_t(now - lastAt) < kInterval) return;
  lastAt = now;
  // No SD reads, gauge sampling or writes while the host owns the card. Keep
  // this guard as well as main.cpp's exclusive-storage early return.
  if (Storage.usbDriveState() != UsbDriveState::Unsupported) return;
  RenderLock render(RenderLock::Mode::Try);
  if (!render.ownsLock()) return;  // Never wait for or interrupt a render.

  const auto internal = HalMemory::getInternalHeap();
  const auto dma = HalMemory::getInternalDmaHeap();
  const auto psram = HalMemory::getPsramHeap();
  const BatteryMonitor battery;
  const auto sample = battery.readPowerSnapshot();  // Read-only raw BQ registers.
  int charger = -1;
  if (metalio_i2c1_lock(20)) {
    uint8_t value = 0;
    if (plus_i2c_read(0x6b, 0x1e, &value, 1) == ESP_OK) charger = value;
    metalio_i2c1_unlock();
  }
  int average = 0, minimum = 0, maximum = 0;
  epd_lcd_get_timing(&average, &minimum, &maximum);
  // Only fixed stack scratch. The existing HAL file handle owns its normal
  // filesystem bookkeeping; there is no retained heap arena, timer or task.
  char row[512];
  const int count = snprintf(row, sizeof(row),
      "%lu,%u,%u,%d,%u,%d,%u,%u,%d,%u,%lu,%d,%d,%d,"
      "%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu\n",
      static_cast<unsigned long>(now), sample.mv, sample.mvOk, sample.current, sample.currentOk,
      sample.average, sample.averageOk, getCpuFrequencyMhz(), charger,
      epd_board_metalio_eink4_plus_tps_ok(), static_cast<unsigned long>(epd_lcd_get_first_fault()),
      average, minimum, maximum,
      static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.minFreeBytes),
      static_cast<unsigned>(internal.largestBlockBytes), static_cast<unsigned>(dma.freeBytes),
      static_cast<unsigned>(dma.minFreeBytes), static_cast<unsigned>(dma.largestBlockBytes),
      static_cast<unsigned>(psram.freeBytes), static_cast<unsigned>(psram.minFreeBytes),
      static_cast<unsigned>(psram.largestBlockBytes), static_cast<unsigned long>(previousWriteMs));
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(row)) { stopped = true; return; }

  const uint32_t started = millis();
  auto file = Storage.open(kPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) { stopped = true; LOG_ERR("FIELD", "Recording open failed; disabled until restart"); return; }
  const size_t extra = sessionHeaderWritten ? 0 : sizeof(kHeader) - 1;
  const auto size = file.fileSize();
  if (size > kLimit || extra + static_cast<size_t>(count) > kLimit - size) {
    stopped = true;
    LOG_INF("FIELD", "Recording full; kept existing data");
    return;
  }
  if (!sessionHeaderWritten && file.write(kHeader, sizeof(kHeader) - 1) != sizeof(kHeader) - 1) {
    stopped = true;
    return;
  }
  if (file.write(row, static_cast<size_t>(count)) != static_cast<size_t>(count)) stopped = true;
  // Close before USB can take ownership and bound data loss if power is lost.
  if (!file.close()) stopped = true;
  previousWriteMs = uint32_t(millis() - started);
  if (!sessionHeaderWritten) LOG_INF("FIELD", "Offline recording active: %s", kPath);
  sessionHeaderWritten = true;
}
#endif
