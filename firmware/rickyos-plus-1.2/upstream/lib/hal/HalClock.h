#pragma once

#include <Arduino.h>
#include <BoardConfig.h>
#include <Rtc.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalWifiAwakeLease.h>
#endif

#include <cstdint>
#include <ctime>

enum class ClockSyncState : uint8_t {
  Idle,
  Syncing,
  Succeeded,
  Failed,
};

class HalClock;
extern HalClock halClock;

class HalClock {
  Rtc _sdkRtc;
  bool _rtcAvailable = false;
  bool _autoSyncEnabled = true;
  bool _wifiWasConnected = false;
  bool _sntpInitialized = false;
  bool _useChinaServers = false;
  ClockSyncState _syncState = ClockSyncState::Idle;
  unsigned long _lastSyncMs = 0;
  unsigned long _lastDiagnosticMs = 0;
  unsigned long _syncStartedMs = 0;
#if FREEINK_DEVICE_METALIO_PLUS
  HalWifiAwakeLease _wifiAwakeLease;
#endif
  bool _wifiPowerSaveHeld = false;
  bool _httpsAttempted = false;

  bool restoreSystemTimeFromRtc();
  bool updateRtcFromSystemTime();
  bool startSntp();
  void stopSntp();
  void completeSync();

 public:
  // The POSIX UTC system clock is the runtime source on every device. An
  // external RTC, when present, is used only to restore time after power loss.
  void begin();
  void update();
  bool isAvailable() const { return _rtcAvailable; }
  void setTimezone(const char* posixTz);
  bool localTime(struct tm& out) const;
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;
  bool syncFromNTP() { return syncNow(); }

  time_t nowUtc() const;
  bool hasValidTime() const;
  bool setUtcTime(time_t epoch);

  void setAutoSyncEnabled(bool enabled);
  void setUseChinaServers(bool enabled) { _useChinaServers = enabled; }
  bool requestSync();
  void cancelSync();
  bool syncNow(uint32_t timeoutMs = 10000);
  ClockSyncState syncState() const { return _syncState; }
};
