#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>
#include <atomic>

#include "BatteryDisplayFilter.h"
#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  mutable BatteryDisplayFilter batteryDisplay;
  mutable std::atomic_flag batteryPollBusy = ATOMIC_FLAG_INIT;
  mutable std::atomic<uint16_t> batteryDisplayPercent{101};  // Unknown until the first valid sample.
  mutable uint32_t batteryDiagnosticAt = 0;
  mutable bool batteryDiagnosticLogged = false;
#endif
#if FREEINK_DEVICE_METALIO_PLUS
  uint32_t batteryAwakePollAt = 0;
  bool batteryAwakePolled = false;
#endif

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect lock mode, clock transitions, and isLowPower

 public:
  // PowerButtonHeld: woken by the key, which is still down; the caller's own key
  // handling sees the rest of that press (Read Pico RickyOS).
  enum class LightSleepWakeReason : uint8_t { Timer, PowerButton, PowerButtonHeld, Failed };

#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_IDLE_FREQ_MHZ)
  // Isolated awake-idle experiment. The S31/40 MHz XTAL path avoids changing
  // sleep/PSRAM retention; ordinary Plus builds retain the existing 80 MHz.
  static_assert(RICKYOS_PLUS_IDLE_FREQ_MHZ == 40 || RICKYOS_PLUS_IDLE_FREQ_MHZ == 80,
                "Plus idle frequency must be 40 or 80 MHz");
#if RICKYOS_PLUS_IDLE_FREQ_MHZ == 40 && !CONFIG_IDF_TARGET_ESP32S31
#error "The 40 MHz Plus idle candidate requires ESP32-S31"
#endif
  static constexpr int LOW_POWER_FREQ = RICKYOS_PLUS_IDLE_FREQ_MHZ;
#elif BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // True when this board can run a light-sleep cycle at all. Only the
  // runtime-selected X3 profiles and Read Pico have validated standby wake
  // wiring. Plus has an experimental board-owned side-key/timer path. Read Pico's is valid whenever the
  // FCA9555 came up, and its wake set is armed in HalPowerManager.cpp (IOE INT#
  // GPIO41, low level) rather than from a GPIO power pin.
  bool canStandbyLightSleep(const HalGPIO& gpio) const;

  // Enter one light-sleep cycle. GPIO and timer wake sources are removed before returning.
  LightSleepWakeReason lightSleepFor(uint32_t seconds) const;

  // Standby's side-key bookkeeping (Read Pico / Plus). A Standby that is just opening forgets
  // the key events seen so far; main.cpp reports while it is judging a press, and
  // Standby does not light-sleep in the middle of one.
  static void beginStandbyKeyWatch();
  static void setSideKeyBusy(bool busy);
  static bool sideKeyBusy();

  // Get battery percentage (0-100); Pico/Plus return 101 before any valid read.
  uint16_t getBatteryPercentage() const;

#if FREEINK_DEVICE_METALIO_PLUS
  // Main-loop only: observe charge qualification without a paint or a wake timer.
  void pollBatteryWhileAwake();
#endif

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
