#include "RickyFreeSpace.h"
#include <HalStorage.h>

#ifndef SIMULATOR
#include <Logging.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <memory>
#include <new>
#endif

namespace RickyFreeSpace {
#ifdef SIMULATOR
// The host filesystem answers at once; there is no FAT to walk.
bool measure(uint64_t& totalBytes, uint64_t& freeBytes) { return Storage.getSpace(totalBytes, freeBytes); }
#else
namespace {
constexpr uint32_t kSectorBytes = 512;
// 8 KB per read: one SD command, small enough to stay in internal (DMA) RAM, and
// short enough that a page waiting for the card barely notices.
constexpr uint32_t kSliceSectors = 16;

struct Geometry {
  uint8_t fatType = 0;  // 16, 32, or 64 (exFAT)
  uint32_t clusters = 0;
  uint32_t sectorsPerCluster = 0;
  uint32_t tableStart = 0;  // FAT16/32: first FAT; exFAT: the cluster bitmap at the heap start
  FsBlockDeviceInterface* device = nullptr;
};

bool readGeometry(Geometry& geometry) {
  HalStorage::StorageLock lock;
  auto& card = SDCardManager::getInstance();
  if (!card.ready()) return false;
  FsVolume& volume = card.mountedVolume();
  geometry.fatType = volume.fatType();
  geometry.clusters = volume.clusterCount();
  geometry.sectorsPerCluster = volume.sectorsPerCluster();
  // SdFat's own exFAT count makes the same assumption: the bitmap opens the heap.
  geometry.tableStart = geometry.fatType == 64 ? volume.dataStartSector() : volume.fatStartSector();
  geometry.device = card.rawBlockDevice();
  return geometry.device && geometry.clusters > 0 && geometry.sectorsPerCluster > 0 &&
         (geometry.fatType == 16 || geometry.fatType == 32 || geometry.fatType == 64);
}

// Free clusters among the table bytes [base, base + size) of one slice.
uint32_t countSlice(const Geometry& geometry, const uint8_t* data, const uint64_t base, const uint32_t size) {
  uint32_t found = 0;
  if (geometry.fatType == 64) {
    // One bit per cluster, set = in use; bits past the last cluster are padding.
    for (uint32_t i = 0; i < size; ++i) {
      const uint64_t firstBit = (base + i) * 8;
      if (firstBit >= geometry.clusters) break;
      const int valid = static_cast<int>(std::min<uint64_t>(8, geometry.clusters - firstBit));
      const uint8_t mask = static_cast<uint8_t>((1u << valid) - 1);
      found += static_cast<uint32_t>(valid - __builtin_popcount(data[i] & mask));
    }
    return found;
  }
  // FAT16/32: entries 0 and 1 are reserved; clusters 2..clusters+1 follow.
  const uint32_t entryBytes = geometry.fatType == 32 ? 4 : 2;
  const uint64_t lastEntry = static_cast<uint64_t>(geometry.clusters) + 2;
  for (uint32_t offset = 0; offset + entryBytes <= size; offset += entryBytes) {
    const uint64_t entry = (base + offset) / entryBytes;
    if (entry < 2) continue;
    if (entry >= lastEntry) break;
    const uint8_t* p = data + offset;
    const uint32_t value = entryBytes == 4 ? (static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                                              static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24) &
                                                 0x0FFFFFFFu
                                           : static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8;
    if (value == 0) ++found;
  }
  return found;
}
}  // namespace

bool measure(uint64_t& totalBytes, uint64_t& freeBytes) {
  totalBytes = 0;
  freeBytes = 0;
  Geometry geometry;
  // FAT12 (tiny cards) or no card: SdFat's own one-pass count is short there.
  if (!readGeometry(geometry)) return Storage.getSpace(totalBytes, freeBytes);
  std::unique_ptr<uint8_t[]> slice(new (std::nothrow) uint8_t[kSliceSectors * kSectorBytes]);
  if (!slice) return Storage.getSpace(totalBytes, freeBytes);

  const uint64_t tableBytes = geometry.fatType == 64
                                  ? (static_cast<uint64_t>(geometry.clusters) + 7) / 8
                                  : (static_cast<uint64_t>(geometry.clusters) + 2) * (geometry.fatType == 32 ? 4 : 2);
  const uint32_t tableSectors = static_cast<uint32_t>((tableBytes + kSectorBytes - 1) / kSectorBytes);
  const uint32_t started = millis();
  uint64_t freeClusters = 0;
  for (uint32_t sector = 0; sector < tableSectors; sector += kSliceSectors) {
    const uint32_t count = std::min(kSliceSectors, tableSectors - sector);
    {
      HalStorage::StorageLock lock;
      // The card can leave mid-walk (USB drive, removal): stop rather than read a stale device.
      if (!SDCardManager::getInstance().ready() || SDCardManager::getInstance().rawBlockDevice() != geometry.device ||
          !geometry.device->readSectors(geometry.tableStart + sector, slice.get(), count)) {
        LOG_ERR("STOR", "Free-space walk stopped at sector %u", static_cast<unsigned>(sector));
        return false;
      }
    }
    freeClusters +=
        countSlice(geometry, slice.get(), static_cast<uint64_t>(sector) * kSectorBytes, count * kSectorBytes);
    taskYIELD();  // let a page that waited on the lock take the card now
  }
  const uint64_t clusterBytes = static_cast<uint64_t>(geometry.sectorsPerCluster) * kSectorBytes;
  totalBytes = static_cast<uint64_t>(geometry.clusters) * clusterBytes;
  freeBytes = std::min<uint64_t>(freeClusters, geometry.clusters) * clusterBytes;
  LOG_INF("STOR", "Free space: FAT%u, %u sectors in %u ms, %u MB free", static_cast<unsigned>(geometry.fatType),
          static_cast<unsigned>(tableSectors), static_cast<unsigned>(millis() - started),
          static_cast<unsigned>(freeBytes >> 20));
#if FREEINK_READPICO_DIAGNOSTICS
  // Cross-check against SdFat's own one-pass count (holds the lock throughout).
  uint64_t sdfatTotal = 0, sdfatFree = 0;
  const uint32_t sdfatStarted = millis();
  if (Storage.getSpace(sdfatTotal, sdfatFree)) {
    LOG_INF("STOR", "SdFat count: %u MB free in %u ms (%s)", static_cast<unsigned>(sdfatFree >> 20),
            static_cast<unsigned>(millis() - sdfatStarted), sdfatFree == freeBytes ? "match" : "MISMATCH");
  }
#endif
  return true;
}
#endif
}  // namespace RickyFreeSpace

