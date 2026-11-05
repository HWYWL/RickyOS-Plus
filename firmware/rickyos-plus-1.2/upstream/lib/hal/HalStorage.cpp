#include "HalStorage.h"

#include <Arduino.h>
#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <Memory.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <atomic>
#include <cstring>
#include <cassert>

#if FREEINK_DEVICE_READPICO
#include <BoardReadPico.h>
#endif

#define SDCard SDCardManager::getInstance()

namespace {
#if FREEINK_CAP_USB_MSC
freeink::UsbMassStorage usbMassStorage;

// USB 传书 hands the raw card to the host. A failed sector transfer ends the session
// (the activity disconnects and restarts), and with the serial port taken by USB it
// left no trace. Each failure is logged (it reaches the retained log ring, printed on
// the next boot) and retried: a transient card or bus error then costs a retry instead
// of the whole transfer.
class RetryingBlockDevice final : public FsBlockDeviceInterface {
 public:
  void attach(FsBlockDeviceInterface* inner) { inner_ = inner; }
  void end() override { inner_->end(); }
  bool isBusy() override { return inner_->isBusy(); }
  Sector_t sectorCount() override { return inner_->sectorCount(); }
  bool syncDevice() override { return inner_->syncDevice(); }
  bool readSector(Sector_t sector, uint8_t* dst) override {
    return retry("read", sector, 1, [&] { return inner_->readSector(sector, dst); });
  }
  bool readSectors(Sector_t sector, uint8_t* dst, size_t ns) override {
    return retry("read", sector, ns, [&] { return inner_->readSectors(sector, dst, ns); });
  }
  bool writeSector(Sector_t sector, const uint8_t* src) override {
    return retry("write", sector, 1, [&] { return inner_->writeSector(sector, src); });
  }
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t ns) override {
    return retry("write", sector, ns, [&] { return inner_->writeSectors(sector, src, ns); });
  }

  UsbDriveStats stats() const {
    UsbDriveStats s;
    s.reads = reads_.load();
    s.writes = writes_.load();
    s.sectorsWritten = sectorsWritten_.load();
    s.retries = retries_.load();
    s.failures = failures_.load();
    s.maxOpMs = maxOpMs_.load();
    const uint32_t started = opStartedAt_.load();
    s.inFlightMs = started ? millis() - started : 0;
    const uint32_t last = lastOpAt_.load();
    s.idleMs = last ? millis() - last : 0;
    return s;
  }
  void resetStats() {
    for (auto* counter : {&reads_, &writes_, &sectorsWritten_, &retries_, &failures_, &maxOpMs_, &opStartedAt_, &lastOpAt_})
      counter->store(0);
  }

 private:
  template <typename Op>
  bool retry(const char* what, const Sector_t sector, const size_t count, Op&& op) {
    constexpr int kAttempts = 4;
    const bool write = what[0] == 'w';
    (write ? writes_ : reads_).fetch_add(1);
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
      const uint32_t started = millis();
      opStartedAt_.store(started ? started : 1);
      const bool ok = op();
      opStartedAt_.store(0);
      lastOpAt_.store(millis());
      const uint32_t took = millis() - started;
      if (took > maxOpMs_.load()) maxOpMs_.store(took);
      if (took >= 1000) {
        LOG_ERR("USB", "%s sector %lu x%u took %lu ms", what, static_cast<unsigned long>(sector),
                static_cast<unsigned>(count), static_cast<unsigned long>(took));
      }
      if (ok) {
        if (write) sectorsWritten_.fetch_add(static_cast<uint32_t>(count));
        if (attempt > 1) {
          LOG_ERR("USB", "%s sector %lu x%u ok on attempt %d", what, static_cast<unsigned long>(sector),
                  static_cast<unsigned>(count), attempt);
        }
        return true;
      }
      LOG_ERR("USB", "%s sector %lu x%u failed (attempt %d)", what, static_cast<unsigned long>(sector),
              static_cast<unsigned>(count), attempt);
      retries_.fetch_add(1);
      delay(attempt * 20);
    }
    failures_.fetch_add(1);
    return false;
  }

  FsBlockDeviceInterface* inner_ = nullptr;
  std::atomic<uint32_t> reads_{0}, writes_{0}, sectorsWritten_{0}, retries_{0}, failures_{0}, maxOpMs_{0};
  std::atomic<uint32_t> opStartedAt_{0}, lastOpAt_{0};
};
RetryingBlockDevice usbBlockDevice;

// Every SCSI command the host sends, polls included: a host that dropped the disk
// (macOS does when a power transition times out behind a long read burst) sends
// none, and nothing else tells that apart from a quiet but mounted disk.
std::atomic<uint32_t> lastHostCommandAt{0};
std::atomic<uint32_t> hostStartStops{0};
std::atomic<uint8_t> lastStartStop{0};
void noteHostCommand() {
  const uint32_t now = millis();
  lastHostCommandAt.store(now ? now : 1);
}
#endif
}  // namespace

#if FREEINK_CAP_USB_MSC
// TinyUSB's MSC callbacks, wrapped at link time (-Wl,--wrap in the env) around the
// Arduino core's definitions.
extern "C" {
bool __real_tud_msc_test_unit_ready_cb(uint8_t lun);
bool __real_tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject);
int32_t __real_tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize);
bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier);

bool __wrap_tud_msc_test_unit_ready_cb(uint8_t lun) {
  noteHostCommand();
  return __real_tud_msc_test_unit_ready_cb(lun);
}

bool __wrap_tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  noteHostCommand();
  hostStartStops.fetch_add(1);
  lastStartStop.store(static_cast<uint8_t>((power_condition << 2) | (start ? 1 : 0) | (load_eject ? 2 : 0)));
  return __real_tud_msc_start_stop_cb(lun, power_condition, start, load_eject);
}

// The core answers every command it does not know with a buffer of garbage (its
// error code is stored in a uint16_t), so SYNCHRONIZE CACHE and MODE SENSE(10),
// both of which macOS sends, are answered here.
int32_t __wrap_tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
  noteHostCommand();
#if FREEINK_DEVICE_METALIO_PLUS
  // The S31 backend owns these commands and actually flushes the SD on SYNC CACHE.
  return __real_tud_msc_scsi_cb(lun, scsi_cmd, buffer, bufsize);
#else
  switch (scsi_cmd[0]) {
    case 0x35:  // SYNCHRONIZE CACHE(10): every write reaches the card before it completes
    case 0x91:  // SYNCHRONIZE CACHE(16)
      return 0;
    case 0x5A: {  // MODE SENSE(10): header only, writable, no block descriptors
      const uint8_t header[8] = {0, 6, 0, 0, 0, 0, 0, 0};
      const uint16_t length = bufsize < sizeof(header) ? bufsize : sizeof(header);
      memcpy(buffer, header, length);
      return length;
    }
    default:
      break;
  }
  const int32_t result = __real_tud_msc_scsi_cb(lun, scsi_cmd, buffer, bufsize);
  if (result != 0) {
    tud_msc_set_sense(lun, 0x05, 0x20, 0x00);  // ILLEGAL REQUEST, invalid command
    return -1;
  }
  return 0;
#endif
}
}
#endif

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  const bool mounted = SDCard.begin();
#if FREEINK_DEVICE_READPICO
  if (!mounted) {
    // Read Pico's slot is 1-bit SDMMC (CLK38/CMD42/D0) with no ESP-side detect
    // pin: card detect lives on the FCA9555 (P0.6, active-low) and the mount is
    // by attempt — the SDK already re-runs the whole mount after a 200 ms settle,
    // which is the vendor's first-clock-negotiation workaround
    // (SdmmcBlockDevice.cpp; read_pico_sd.c). The CD line is a hint for this
    // message only, read AFTER the mount so a card that came up fine is never
    // rejected by it. Unreadable-card handling belongs to the caller (src/main.cpp
    // shows the SD error screen and returns); nothing here aborts.
    const bool cardDetect = cardDetectAsserted();
    LOG_ERR("STORAGE", "SD mount failed; card detect %s",
            cardDetect ? "asserted (card present but unreadable)" : "released or unreadable");
  }
#endif
  return mounted;
}

bool HalStorage::ready() const {
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  // Never wait here: a caller may hold the render lock the standby task wants.
  if (standbyRemountPending && xSemaphoreTakeRecursive(storageMutex, 0) == pdTRUE) {
    recoverAfterStandby();
    xSemaphoreGiveRecursive(storageMutex);
  }
#endif
  return SDCard.ready();
}

bool HalStorage::cardDetectAsserted() const {
#if FREEINK_DEVICE_READPICO
  // P0.6, active-low; false on any I2C failure (see the header contract).
  return BoardReadPico::sdCardPresent();
#else
  return false;
#endif
}

// For the rest of the methods, we acquire the mutex to ensure thread safety

HalStorage::StorageLock::StorageLock() {
  xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (standbyRemountPending) recoverAfterStandby();
#endif
}

HalStorage::StorageLock::~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }

bool HalStorage::getSpace(uint64_t& totalBytes, uint64_t& freeBytes) {
  StorageLock lock;
  if (SDCard.getSpace(totalBytes, freeBytes)) return true;
  LOG_ERR("STORAGE", "Unable to query SD filesystem space");
  return false;
}

void HalStorage::prepareForDeepSleep() {
  StorageLock lock;
  SDCard.shutdown();
}

#if FREEINK_CAP_USB_MSC && !FREEINK_SD_SDMMC
#error "USB Drive requires an SDMMC-backed storage profile"
#endif

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  // Spotlight would otherwise index every book over the slow USB link, reading the
  // card at full speed in the background for as long as the disk stays mounted.
  constexpr char kNeverIndex[] = "/.metadata_never_index";
  if (!SDCard.exists(kNeverIndex)) {
    HalFile marker;
    if (openFileForWrite("USB", kNeverIndex, marker)) marker.close();
  }
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SDMMC filesystem");
    return false;
  }

  usbBlockDevice.attach(blockDevice);
  usbBlockDevice.resetStats();
  lastHostCommandAt.store(0);
  hostStartStops.store(0);
  if (!usbMassStorage.begin(&usbBlockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
#if FREEINK_DEVICE_METALIO_PLUS
    // The card/host remains initialized after raw handoff; remount only SdFat,
    // without allocating a second SDMMC session after failed USB startup.
    if (!SDCard.remountFilesystemAfterRawAccess()) {
#else
    if (!SDCard.begin()) {
#endif
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.disconnectHost();
#else
  return false;
#endif
}

bool HalStorage::usbDriveHostSuspended() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.hostSuspended();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  const UsbDriveStats s = usbBlockDevice.stats();
  LOG_INF("USB", "session: %lu reads, %lu writes, %lu retries, %lu failures, slowest %lu ms",
          static_cast<unsigned long>(s.reads), static_cast<unsigned long>(s.writes),
          static_cast<unsigned long>(s.retries), static_cast<unsigned long>(s.failures),
          static_cast<unsigned long>(s.maxOpMs));
  usbMassStorage.end();
#endif
}

UsbDriveStats HalStorage::usbDriveStats() const {
#if FREEINK_CAP_USB_MSC
  UsbDriveStats s = usbBlockDevice.stats();
  s.suspended = usbMassStorage.hostSuspended();
  const uint32_t lastCommand = lastHostCommandAt.load();
  s.hostQuietMs = lastCommand ? millis() - lastCommand : 0;
  s.startStops = hostStartStops.load();
  s.lastStartStop = lastStartStop.load();
  return s;
#else
  return {};
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  switch (usbMassStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

// Composed of already-locked HalStorage/HalFile operations; needs no
// StorageLock of its own.
bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  if (file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  // string growth is a bare allocation under -fno-exceptions; probe first so
  // a large file on a fragmented heap fails soft instead of abort()ing.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 512) {
    LOG_ERR(moduleName, "readFileToString OOM: %u bytes for %s", static_cast<unsigned>(size), path.c_str());
    return false;
  }
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  StorageLock lock;
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  invalidateStandbyPath(path);
#endif
  return SDCard.writeFile(path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile, const char* path = nullptr, oflag_t flags = O_RDONLY) : file(std::move(fsFile)) {
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
    // No extra allocation: metadata lives in the existing Impl allocation, only
    // in this Plus experiment. C3 and ordinary Plus handle sizes are unchanged.
    if (path && path[0] == '/' && flags == O_RDONLY && strlen(path) < sizeof(resumePath))
      memcpy(resumePath, path, strlen(path) + 1);
    next = head;
    head = this;
#else
    (void)path;
    (void)flags;
#endif
  }
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in docs/engineering/architecture-and-patterns.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
    for (Impl** link = &head; *link; link = &(*link)->next) {
      if (*link == this) { *link = next; break; }
    }
#endif
  }
  FsFile file;
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  inline static Impl* head = nullptr;
  Impl* next = nullptr;
  char resumePath[320]{};
  uint64_t resumePosition = 0;
  uint64_t resumeSize = 0;
  bool parked = false;

  static bool resumeAll() {
    bool ok = true;
    for (auto* entry = head; entry; entry = entry->next) {
      if (!entry->parked) continue;
      entry->file = SDCard.open(entry->resumePath, O_RDONLY);
      const bool restored = entry->file && !entry->file.isDirectory() &&
          entry->file.fileSize() == entry->resumeSize && entry->file.seekSet(entry->resumePosition);
      if (!restored) {
        entry->file.close();
        LOG_ERR("STORAGE", "Standby file restore failed: %s", entry->resumePath);
        ok = false;
      }
      entry->parked = false;
    }
    return ok;
  }
  static bool suspendAll() {
    // Validate the entire set before closing anything. Writers, directory
    // iterators, anonymous openNextFile handles and renamed paths are unsafe.
    for (auto* entry = head; entry; entry = entry->next) {
      if (!entry->file.isOpen()) continue;
      if (!entry->resumePath[0] || entry->file.isDirectory() || !SDCard.exists(entry->resumePath)) {
        LOG_ERR("STORAGE", "Standby blocked by unsupported live file handle");
        return false;
      }
      entry->resumePosition = entry->file.position();
      entry->resumeSize = entry->file.fileSize();
    }
    for (auto* entry = head; entry; entry = entry->next) {
      if (!entry->file.isOpen()) continue;
      entry->parked = true;
      if (!entry->file.close()) {
        resumeAll();
        return false;
      }
    }
    return true;
  }
#endif
};

#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
void HalStorage::invalidateStandbyPath(const char* path) {
  const size_t length = strlen(path);
  for (auto* entry = HalFile::Impl::head; entry; entry = entry->next) {
    if (strncmp(entry->resumePath, path, length) == 0 &&
        (entry->resumePath[length] == '\0' || entry->resumePath[length] == '/')) entry->resumePath[0] = '\0';
  }
}

// Mount the card again after a standby park. Some cards answer late after their
// bus was held low through sleep, so a failed mount is retried after a pause.
// Returns the number of mounts it took, 0 when the card did not come back.
uint32_t HalStorage::remountForStandby() {
  static constexpr uint32_t kPauseMs[] = {0, 150, 500};
  for (uint32_t attempt = 0; attempt < sizeof(kPauseMs) / sizeof(kPauseMs[0]); ++attempt) {
    if (kPauseMs[attempt] != 0) delay(kPauseMs[attempt]);
    if (SDCard.begin()) return attempt + 1;
  }
  return 0;
}

void HalStorage::recoverAfterStandby() {
  static bool recovering = false;
  if (recovering) return;
  if (SDCard.ready()) {
    standbyRemountPending = false;
    return;
  }
  const uint32_t now = millis();
  if (standbyRemountTriedMs != 0 && uint32_t(now - standbyRemountTriedMs) < 2000) return;
  recovering = true;
  standbyRemountTriedMs = now;
  const uint32_t mounts = remountForStandby();
  standbyRemountTriedMs = millis();
  if (mounts != 0) {
    standbyRemountPending = false;
    HalFile::Impl::resumeAll();
    LOG_INF("STORAGE", "SD back after standby (late remount, %u tries)", static_cast<unsigned>(mounts));
  }
  recovering = false;
}

bool HalStorage::StandbySession::suspend() {
  if (standbyParkDisabled) return false;
  if (suspended_ || !SDCard.ready() || !HalFile::Impl::suspendAll()) return false;
  auto* device = SDCard.rawBlockDevice();
  if (!device || !device->syncDevice()) {
    HalFile::Impl::resumeAll();
    return false;
  }
  SDCard.shutdown();
  suspended_ = true;
  return true;
}
bool HalStorage::StandbySession::resume() {
  if (!suspended_) return true;
  suspended_ = false;
  const uint32_t mounts = remountForStandby();
  if (mounts != 1) {
    // This card is slow or unreliable after a park: keep it mounted through
    // every later Standby of this boot.
    standbyParkDisabled = true;
    LOG_ERR("STORAGE", "Standby SD remount %s after %u tries; SD stays on in Standby until restart",
            mounts ? "succeeded" : "failed", static_cast<unsigned>(mounts ? mounts : 3));
  }
  if (mounts == 0) {
    // Parked handles stay parked: the next storage use mounts the card again
    // and reopens them at their positions.
    standbyRemountPending = true;
    standbyRemountTriedMs = millis();
    return false;
  }
  return HalFile::Impl::resumeAll();
}
HalStorage::StandbySession::~StandbySession() {
  if (suspended_) resume();
}
#endif

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (oflag != O_RDONLY) invalidateStandbyPath(path);
#endif
  FsFile fsFile = SDCard.open(path, oflag);
  if (!fsFile) return {};

  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile), path, oflag);
  if (!impl) {
    LOG_ERR("HAL", "OOM: HalFile::Impl (%u bytes)", static_cast<unsigned>(sizeof(HalFile::Impl)));
    return {};
  }
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) {
  StorageLock lock;
  const bool ok = SDCard.remove(path);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (ok) invalidateStandbyPath(path);
#endif
  return ok;
}
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  StorageLock lock;
  const bool ok = SDCard.rename(oldPath, newPath);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (ok) invalidateStandbyPath(oldPath);
  if (ok) invalidateStandbyPath(newPath);
#endif
  return ok;
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  StorageLock lock;
  const bool ok = SDCard.replaceFile(tmpPath, path);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (ok) invalidateStandbyPath(tmpPath);
  if (ok) invalidateStandbyPath(path);
#endif
  return ok;
}

bool HalStorage::rmdir(const char* path) {
  StorageLock lock;
  const bool ok = SDCard.rmdir(path);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (ok) invalidateStandbyPath(path);
#endif
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  if (!SDCard.openFileForRead(moduleName, path, fsFile)) {
    file = HalFile();
    return false;
  }
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile), path, O_RDONLY);
  if (!impl) {
    LOG_ERR(moduleName, "OOM: HalFile::Impl (%u bytes)", static_cast<unsigned>(sizeof(HalFile::Impl)));
    file = HalFile();
    return false;
  }
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  invalidateStandbyPath(path);
#endif
  if (!SDCard.openFileForWrite(moduleName, path, fsFile)) {
    file = HalFile();
    return false;
  }
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile), path, O_RDWR | O_CREAT | O_TRUNC);
  if (!impl) {
    LOG_ERR(moduleName, "OOM: HalFile::Impl (%u bytes)", static_cast<unsigned>(sizeof(HalFile::Impl)));
    file = HalFile();
    return false;
  }
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
#define HAL_FILE_METADATA_LOCK HalStorage::StorageLock lock;
#else
#define HAL_FILE_METADATA_LOCK
#endif

#define HAL_FILE_FORWARD_CALL(method, ...) \
  HAL_FILE_METADATA_LOCK                  \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl || !impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
bool HalFile::truncate(const uint64_t length) { HAL_FILE_WRAPPED_CALL(truncate, length); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const uint8_t* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  const bool ok = impl->file.rename(newPath);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  if (ok) {
    // The path may name other open readers too. Copy before invalidating the
    // registry so clearing this entry cannot alter the match for later entries.
    char oldPath[sizeof(impl->resumePath)];
    memcpy(oldPath, impl->resumePath, sizeof(oldPath));
    if (oldPath[0]) HalStorage::invalidateStandbyPath(oldPath);
    HalStorage::invalidateStandbyPath(newPath);
    impl->resumePath[0] = '\0';
  }
#endif
  return ok;
}
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
// Closing an unopened handle is a no-op, not misuse. openFileForRead()/openFileForWrite()
// deliberately hand back a null handle when the open fails (file = HalFile()), and the
// tree-wide pattern for member files is to close them unconditionally -- so every caller
// that closes a member whose open failed lands here with impl == nullptr. The failure has
// already been reported by that open* return value, and asserting on it turns a
// recoverable error (a book whose container is not a readable ZIP, a full or missing SD
// card) into a panic. Field report on a Read Pico: assert here while opening a book, with
// "[ZIP] EOCD signature not found in zip file" immediately before it.
bool HalFile::close() {
  HalStorage::StorageLock lock;
  if (!impl) return true;
  return impl->file.close();
}
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  FsFile fsFile = impl->file.openNextFile();
  if (!fsFile) return {};

  auto nextImpl = makeUniqueNoThrow<Impl>(std::move(fsFile));
  if (!nextImpl) {
    LOG_ERR("HAL", "OOM: HalFile::Impl (%u bytes)", static_cast<unsigned>(sizeof(Impl)));
    return {};
  }
  return HalFile(std::move(nextImpl));
}
bool HalFile::isOpen() const { HAL_FILE_METADATA_LOCK return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
