#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

struct HeapInfo {
  uint32_t freeBytes;
  uint32_t totalBytes;
  uint32_t largestFreeBlockBytes;
  uint32_t freePsramBytes;
  uint32_t totalPsramBytes;
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

using DeviceId = std::array<uint8_t, 6>;
// Stable across boots, changes with the compiled image even when dev version is unchanged.
std::array<uint8_t, 8> getFirmwareBuildId();
const char* getDeviceModel();
const char* getChipModel();
bool getDeviceId(DeviceId& out);
bool getWifiStationMac(DeviceId& out);
bool getChipTemperatureCelsius(float& out);
uint64_t getUptimeSeconds();
HeapInfo getHeapInfo();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();
// The retained log ring of the session before a software restart (e.g. USB 传书 ending
// in an error, when the serial port was the USB disk), taken at begin(); empty otherwise.
// Printing it is the caller's choice, once a serial host can be listening.
std::string takePreviousSessionLogs();
}  // namespace HalSystem
