#include "HalPowerManager.h"

#include <BoardConfig.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <PlusBoard.h>
#if defined(RICKYOS_PLUS_STATIC_STANDBY)
#include "HalStorage.h"
#endif
#endif
#if FREEINK_DEVICE_READPICO
#include <BoardReadPico.h>
#endif
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <atomic>
#include <cassert>

#include "StandbyDiag.h"

#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
#include <BleKeyboardHost.h>
#endif

#include "HalGPIO.h"
#include "Waveshare397Power.h"

HalPowerManager powerManager;  // Singleton instance

// GPIO13 is the existing Xteink C3 deep-sleep shutdown signal; the X3 profile
// also identifies it as the SD power rail. Its X4 hardware role is unverified.
// Other boards use it for unrelated signals, including X4 Pro display CS.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

namespace {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
std::atomic<bool> g_sideKeyBusy{false};
#endif
#if !FREEINK_DEVICE_READPICO
struct StandbyRetention {
  int8_t pin;
  int activeLevel;
};

StandbyRetention standbyRetention() {
  const auto& board = BoardConfig::ACTIVE;
  switch (board.board) {
    case BoardConfig::Board::XteinkX4:
      return {board.power.latch0, HIGH};
    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      return {board.sd.powerEnable, board.sd.powerActiveHigh ? HIGH : LOW};
    default:
      // All other profiles remain unvalidated.
      return {BoardConfig::PIN_UNASSIGNED, LOW};
  }
}
#endif  // !FREEINK_DEVICE_READPICO

#if FREEINK_DEVICE_READPICO
// Read Pico turns "off" through the CW32L010 PMU, not through the ESP: there is
// no ESP-side deep-sleep wake source on this board (the FCA9555 INT# on GPIO41
// and the CST836U INT# on GPIO43 are not RTC-capable pads, and the real wake
// events — PMU key, AC-in, RTC alarm — belong to the PMU; read-pico.md B9/B10).
// Installing this hook is what keeps freeink::PowerManager::deepSleep() from
// calling esp_deep_sleep_start() and stranding the chip with nothing able to
// wake it. Signature is void() while the board calls return bool, so the failure
// is logged here; a returned hook means the PMU never cut the rail, and the SDK
// then idles instead of sleeping.
void readPicoHostShutdown() {
  // RickyOS power-off is the real off (the reference firmware's APP_SLEEP_OFF): the
  // side key has to be held about a second to turn it back on. Soft sleep woke on a
  // short press, which RickyOS uses for Standby.
  if (BoardReadPico::pmuPowerOff()) return;
  LOG_ERR("PWR", "PMU power-off handoff failed; trying soft sleep");
  if (BoardReadPico::pmuSoftSleep()) return;

  // Nothing else can turn this board off, and there is no wake source to arm, so
  // report it and let the SDK idle rather than entering a wake-less deep sleep.
  LOG_ERR("PWR", "PMU power-off handoff failed; no ESP wake source, idling");
}

// Read Pico light sleep: one LEVEL wake on the FCA9555 INT# (GPIO41, active-low)
// plus the timer, armed through the board-agnostic freeink::PowerManager
// primitives. GPIO41 is a plain digital pad with no RTC capability, which is
// exactly why this is a light-sleep source and can never be a deep-sleep one
// (read-pico.md B9; PowerManager.h isDeepSleepWakePin()).
//
// The accelerometer INT1 (GPIO1, wake-high) is deliberately NOT armed. The
// SC7A20H only asserts INT1 after AOI1/HPIS1 pickup-wake configuration, and
// nothing programs those registers: the SDK IMU exposes begin/read/sleep/wake
// only (Imu.h), CrossMux has no pickup-to-wake feature, and CTRL3 (INT1_CFG)
// stays 0, so INT1 can never assert. Arming a level trigger on a line nothing
// drives would be dead configuration pretending to be a wake source; the
// primitive takes the high mask whenever a pickup-wake feature lands.
// Newest PMU event id handled by the previous wake of this Standby; 0 until the first
// one (a Standby that is just opening). Events after it that are still queued when the
// next sleep starts are presses nobody saw: the clock redraw holds the main loop for
// about 3 s each minute, and a tap in that window used to be dropped by the drain.
uint16_t g_standbyKeyBoundary = 0;

HalPowerManager::LightSleepWakeReason lightSleepReadPico(const uint32_t seconds) {
  using LightSleepWakeReason = HalPowerManager::LightSleepWakeReason;
  constexpr uint64_t kIoeIntMask = 1ULL << READPICO_IOE_INT;

  if (seconds == 0) {
    LOG_ERR("PWR", "Invalid light-sleep request: seconds=0");
    return LightSleepWakeReason::Failed;
  }
  if (!BoardReadPico::ready()) {
    // Without the expander the INT# net has no driver and no reader, so there is
    // no wake source to arm and no way to attribute a wake.
    LOG_ERR("PWR", "FCA9555 is not up; no light-sleep wake source available");
    return LightSleepWakeReason::Failed;
  }

  // Release a pending assertion first: the expander's INT# is open-drain and NOT
  // latched, and reading its Input register is what releases it (fca9555.h).
  // Leaving a stale low on the line would end the sleep immediately.
  // Presses so far were already read as key levels; drop their PMU events so its
  // interrupt line releases and the next press changes INT# again. Left queued,
  // the full FIFO holds that line and no press can end the sleep. The newest event
  // id seen now marks the boundary: after the sleep only a later press counts, so
  // an old event the PMU reports late cannot close Standby by itself.
  bool keyDown = false;
  const uint16_t boundary = g_standbyKeyBoundary;
  const bool queuedPress = BoardReadPico::takePmuKeyPress(keyDown, boundary);
  const uint16_t lastIdBeforeSleep = BoardReadPico::pmuLastEventId();
  STANDBY_DIAG("pre-sleep %us: drained press=%d keyDown=%d int=%d lastId=%u boundary=%u",
               static_cast<unsigned>(seconds), queuedPress, keyDown, BoardReadPico::ioeIntAsserted(), lastIdBeforeSleep,
               boundary);
  g_standbyKeyBoundary = lastIdBeforeSleep;
  if (boundary != 0 && (queuedPress || keyDown)) {
    // A press since the last wake that the main loop never judged: answer it now.
    return keyDown ? LightSleepWakeReason::PowerButtonHeld : LightSleepWakeReason::PowerButton;
  }
  BoardReadPico::clearIoeInt();

  if (!freeink::PowerManager::armLightSleepWakeupLevels(kIoeIntMask, 0)) {
    LOG_ERR("PWR", "Failed to arm the GPIO%d light-sleep wake", READPICO_IOE_INT);
    return LightSleepWakeReason::Failed;
  }
  const esp_err_t timerError = esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  if (timerError != ESP_OK) {
    freeink::PowerManager::clearLightSleepWakeup(kIoeIntMask);
    LOG_ERR("PWR", "Failed to arm the light-sleep timer: %d", static_cast<int>(timerError));
    return LightSleepWakeReason::Failed;
  }

  const esp_err_t sleepError = esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  freeink::PowerManager::clearLightSleepWakeup(kIoeIntMask);
  const esp_err_t timerCleanupError = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  if (sleepError != ESP_OK) {
    LOG_ERR("PWR", "Light sleep failed: %d", static_cast<int>(sleepError));
    return LightSleepWakeReason::Failed;
  }
  if (timerCleanupError != ESP_OK) {
    LOG_ERR("PWR", "Failed to clean up the light-sleep timer: %d", static_cast<int>(timerCleanupError));
    return LightSleepWakeReason::Failed;
  }

  // The PMU's key event is authoritative, whatever ended the sleep: a press can
  // also land just before a timer wake (read_pico main/sleep.c does the same).
  const bool wakePress = BoardReadPico::takePmuKeyPress(keyDown, lastIdBeforeSleep);
  STANDBY_DIAG("woke cause=%d press=%d keyDown=%d lastId=%u type=%u", static_cast<int>(cause), wakePress, keyDown,
               BoardReadPico::pmuLastEventId(), BoardReadPico::pmuLastEventType());
  if (wakePress && keyDown) {
    // Still down as the sleep ends: main.cpp's 50 ms key poll tells a tap from a
    // hold, but a quick tap can be over before it looks, and then nothing ended
    // Standby (device: a second press was needed). Watch the key here briefly: let go
    // within kTapWindowMs is a tap; still down hands the hold to main.cpp.
    constexpr unsigned long kTapWindowMs = 500;
    const unsigned long watchStart = millis();
    while (keyDown && millis() - watchStart < kTapWindowMs) {
      delay(20);
      (void)BoardReadPico::takePmuKeyPress(keyDown, BoardReadPico::pmuLastEventId());
    }
    STANDBY_DIAG("wake key %s after %lums", keyDown ? "held" : "released", millis() - watchStart);
  }
  g_standbyKeyBoundary = BoardReadPico::pmuLastEventId();
  if (g_standbyKeyBoundary == 0) g_standbyKeyBoundary = 1;  // 0 means "no wake yet"
  if (wakePress) {
    return keyDown ? LightSleepWakeReason::PowerButtonHeld : LightSleepWakeReason::PowerButton;
  }
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    // Another expander input (card detect, charger): stay awake a while, as before.
    LOG_INF("PWR", "Light sleep ended by an expander input, not the key");
    return LightSleepWakeReason::Failed;
  }
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
      return LightSleepWakeReason::Timer;
    case ESP_SLEEP_WAKEUP_GPIO:
      // The armed line is the expander INT#, i.e. the PMU's own interrupt (power
      // key, AC-in, RTC alarm) or any other expander input change (card detect,
      // PGOOD). Reported as PowerButton because that is the only "an external
      // event ended the sleep" value the enum has; StandbyActivity leaves
      // immersive standby on it, which is the correct response to any of them.
      // No waitForPowerButtonRelease(): the key is PMU-owned and there is no GPIO
      // to poll.
      return LightSleepWakeReason::PowerButton;
    default:
      LOG_ERR("PWR", "Unexpected light-sleep wake cause: %d", static_cast<int>(cause));
      return LightSleepWakeReason::Failed;
  }
}
#endif  // FREEINK_DEVICE_READPICO
}  // namespace

void HalPowerManager::begin() {
#if FREEINK_DEVICE_READPICO
  // Board-owned shutdown path, installed once the board is up (HalGPIO::begin()
  // ran before this). Every other target leaves the hook unset and keeps the
  // existing esp_deep_sleep_start() behaviour byte-for-byte.
  freeink::PowerManager::setHostShutdownHook(&readPicoHostShutdown);
#endif
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }
#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
  // Manual 10 MHz downclocking bypasses the controller's IDF power locks.
  // Protect the entire host lifetime, including scans and reconnect attempts.
  if (BleHid.isRunning()) enabled = false;
#endif
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_IDLE_FREQ_MHZ) && RICKYOS_PLUS_IDLE_FREQ_MHZ == 40
  if (!plus_board::idleCpuFrequencyAllowed()) enabled = false;
#endif

  // Serialize the entire clock transition: Arduino's APB callbacks retain SPI
  // locks between BEFORE/AFTER, so concurrent frequency changes can deadlock.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool targetLowPower = enabled && currentLockMode == None;
  if (isLowPower != targetLowPower) {
    const int targetFrequency = targetLowPower ? LOW_POWER_FREQ : normalFreq;
    if (setCpuFrequencyMhz(targetFrequency)) {
      isLowPower = targetLowPower;
      LOG_DBG("PWR", "CPU frequency now %u MHz", getCpuFrequencyMhz());
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_IDLE_FREQ_MHZ)
      // Only log real transitions in the explicit comparison build. A setting
      // alone is not proof that the CPU actually reached the requested rate.
      LOG_INF("PWR", "idle comparison: target=%d actual=%u MHz idle=%u", targetFrequency,
              getCpuFrequencyMhz(), targetLowPower);
#endif
    } else {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", targetFrequency);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_IDLE_FREQ_MHZ)
      LOG_ERR("PWR", "idle comparison rejected: target=%d actual=%u MHz", targetFrequency,
              getCpuFrequencyMhz());
#endif
    }
  }

  xSemaphoreGive(modeMutex);
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#if defined(ENABLE_SERIAL_LOG) && !FREEINK_DEVICE_METALIO_EINK4 && !FREEINK_DEVICE_READPICO
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  //
  // Metalio and Read Pico are excluded because neither has a USB-powered GPIO
  // wake to protect: their "off" is a board-owned shutdown (M5PM1 / the CW32L010
  // PMU) that drops the rail, and tearing HWCDC down first would swallow the very
  // handoff diagnostics those boards need when the shutdown fails.
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // Keep the existing GPIO13 deep-sleep shutdown behavior unchanged while
    // its exact X4 hardware role remains unverified (the SDK still handles wake).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  gpio.prepareForDeepSleep();
  freeink::PowerManager::powerDownRailsForSleep();

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

void HalPowerManager::beginStandbyKeyWatch() {
#if FREEINK_DEVICE_READPICO
  g_standbyKeyBoundary = 0;
#endif
}

void HalPowerManager::setSideKeyBusy(const bool busy) {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  g_sideKeyBusy.store(busy, std::memory_order_relaxed);
#else
  (void)busy;
#endif
}

bool HalPowerManager::sideKeyBusy() {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  return g_sideKeyBusy.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

bool HalPowerManager::canStandbyLightSleep(const HalGPIO& gpio) const {
#if FREEINK_DEVICE_METALIO_PLUS
  (void)gpio;
  return plus_board::standbySleepReady() && !sideKeyBusy();
#elif FREEINK_DEVICE_READPICO
  // The power key is PMU-owned and has no GPIO, so the generic
  // powerPin/retention test below can never pass here. What this board has is a
  // LEVEL wake on the FCA9555 INT# (GPIO41) plus the sleep timer, and the
  // expander must have come up for that line to mean anything: without it the
  // INT# net is just a floating input and a wake could never be attributed.
  (void)gpio;
  // Not while main.cpp is still judging a side-key press: sleeping then left the
  // answer (leave Standby) waiting for the next wake.
  return BoardReadPico::ready() && !g_sideKeyBusy.load(std::memory_order_relaxed);
#else
  return gpio.isXteinkDevice() && BoardConfig::ACTIVE.input.power >= 0 && standbyRetention().pin >= 0;
#endif
}

HalPowerManager::LightSleepWakeReason HalPowerManager::lightSleepFor(const uint32_t seconds) const {
#if FREEINK_DEVICE_METALIO_PLUS
  if (sideKeyBusy()) return LightSleepWakeReason::Failed;
#if defined(RICKYOS_PLUS_STATIC_STANDBY)
  HalStorage::StandbySession session;
  plus_board::StandbyStorage storage{
      [](void* context) { return static_cast<HalStorage::StandbySession*>(context)->suspend(); },
      [](void* context) { return static_cast<HalStorage::StandbySession*>(context)->resume(); }, &session};
  const auto reason = plus_board::standbyStaticSleep(seconds, storage);
#else
  const auto reason = plus_board::standbyLightSleep(seconds);
#endif
  switch (reason) {
    case plus_board::StandbyWake::Timer: return LightSleepWakeReason::Timer;
    case plus_board::StandbyWake::PowerButton: return LightSleepWakeReason::PowerButton;
    case plus_board::StandbyWake::PowerButtonHeld: return LightSleepWakeReason::PowerButtonHeld;
    default: return LightSleepWakeReason::Failed;
  }
#elif FREEINK_DEVICE_READPICO
  // Read Pico: no GPIO power key, no standby retention rail. The wake set is the
  // FCA9555 INT# (GPIO41) on the LOW level plus the timer, wired through the new
  // freeink::PowerManager light-sleep primitives. GPIO41 is NOT RTC-capable on
  // the S3, so it can only ever be a light-sleep source — never an ext1/deep-sleep
  // one (read-pico.md B9) — and the CST836U INT# (GPIO43) is deliberately NOT
  // armed: the reference firmware wakes on GPIO41 only, so a touch cannot wake
  // this board from light sleep. GPIO43 could technically be a light-sleep source
  // but nothing verifies that the CST836U holds INT# quiet while unread; leaving
  // it unarmed matches the verified behaviour instead of guessing.
  return lightSleepReadPico(seconds);
#else
  const int8_t powerPin = BoardConfig::ACTIVE.input.power;
  const StandbyRetention retention = standbyRetention();
  if (seconds == 0 || powerPin < 0 || retention.pin < 0) {
    LOG_ERR("PWR", "Invalid light-sleep request: seconds=%u powerPin=%d retentionPin=%d",
            static_cast<unsigned>(seconds), powerPin, retention.pin);
    return LightSleepWakeReason::Failed;
  }

  const bool activeHigh = BoardConfig::ACTIVE.input.powerActiveHigh;
  pinMode(powerPin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  if (digitalRead(powerPin) == (activeHigh ? HIGH : LOW)) {
    freeink::PowerManager::waitForPowerButtonRelease();
    return LightSleepWakeReason::PowerButton;
  }

  const gpio_num_t retentionPin = static_cast<gpio_num_t>(retention.pin);
  const auto cleanupLightSleep = [&] {
    esp_err_t cleanupError = gpio_wakeup_disable(static_cast<gpio_num_t>(powerPin));
    const auto keepFirstError = [&](const esp_err_t current) {
      if (cleanupError == ESP_OK && current != ESP_OK) cleanupError = current;
    };
    keepFirstError(gpio_set_intr_type(static_cast<gpio_num_t>(powerPin), GPIO_INTR_DISABLE));
    keepFirstError(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO));
    keepFirstError(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER));
    keepFirstError(gpio_sleep_sel_en(retentionPin));
    return cleanupError;
  };

  // ESP32-C3 isolates ordinary GPIOs in light sleep. Keep the X3 SD rail or
  // X4 battery latch actively driven instead of allowing GPIO13 to float.
  esp_err_t error = gpio_set_level(retentionPin, retention.activeLevel);
  if (error == ESP_OK) error = gpio_set_direction(retentionPin, GPIO_MODE_OUTPUT);
  if (error == ESP_OK) error = gpio_sleep_sel_dis(retentionPin);
  if (error == ESP_OK)
    error =
        gpio_wakeup_enable(static_cast<gpio_num_t>(powerPin), activeHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
  if (error == ESP_OK) error = esp_sleep_enable_gpio_wakeup();
  if (error == ESP_OK) error = esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  if (error != ESP_OK) {
    const esp_err_t cleanupError = cleanupLightSleep();
    LOG_ERR("PWR", "Failed to configure light sleep: %d", static_cast<int>(error));
    if (cleanupError != ESP_OK) LOG_ERR("PWR", "Failed to clean up light sleep: %d", static_cast<int>(cleanupError));
    return LightSleepWakeReason::Failed;
  }

  error = esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const esp_err_t cleanupError = cleanupLightSleep();
  if (cause == ESP_SLEEP_WAKEUP_GPIO) freeink::PowerManager::waitForPowerButtonRelease();
  if (error != ESP_OK) {
    LOG_ERR("PWR", "Light sleep failed: %d", static_cast<int>(error));
    return LightSleepWakeReason::Failed;
  }
  if (cleanupError != ESP_OK) {
    LOG_ERR("PWR", "Failed to clean up light sleep: %d", static_cast<int>(cleanupError));
    return LightSleepWakeReason::Failed;
  }

  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
      return LightSleepWakeReason::Timer;
    case ESP_SLEEP_WAKEUP_GPIO:
      return LightSleepWakeReason::PowerButton;
    default:
      LOG_ERR("PWR", "Unexpected light-sleep wake cause: %d", static_cast<int>(cause));
      return LightSleepWakeReason::Failed;
  }
#endif  // FREEINK_DEVICE_READPICO
}

#if FREEINK_DEVICE_METALIO_PLUS
void HalPowerManager::pollBatteryWhileAwake() {
  const uint32_t now = millis();
  if (batteryAwakePolled && uint32_t(now - batteryAwakePollAt) < 30000) return;
  batteryAwakePolled = true;
  batteryAwakePollAt = now;
  // Reuse the UI's single sampler/cache, including its I2C failure handling.
  // Long blocked/sleeping intervals earn no synthetic observations or catch-up.
  getBatteryPercentage();
#if defined(RICKYOS_PLUS_IDLE_FREQ_MHZ)
  plus_board::traceIdlePower();
#endif
}
#endif

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  // Nonblocking single sampler shared by the UI and main loop.
  if (batteryPollBusy.test_and_set(std::memory_order_acquire)) return batteryDisplayPercent.load();
  struct PollGuard {
    std::atomic_flag& flag;
    ~PollGuard() { flag.clear(std::memory_order_release); }
  } guard{batteryPollBusy};
  const uint32_t now = millis();
  if (batteryDisplay.shouldPoll(now)) {
    const auto status = battery.readStatus();
    BatteryDisplayFilter::Sample sample;
    sample.valid = status.percentageKnown;
    sample.percent = status.percentage;
    sample.source = static_cast<uint8_t>(status.percentageSource);
    sample.chargingKnown = status.chargingKnown;
    sample.charging = status.charging;
    const auto shown = batteryDisplay.update(sample, now);
    batteryDisplayPercent.store(batteryDisplay.known() ? shown : 101);
    if (!batteryDiagnosticLogged || uint32_t(now - batteryDiagnosticAt) >= 60000) {
      batteryDiagnosticLogged = true;
      batteryDiagnosticAt = now;
      LOG_INF("BAT", "valid=%d source=%u raw=%u mv=%u charge=%d/%d shown=%u known=%d",
              sample.valid, sample.source, sample.percent, status.millivolts,
              sample.chargingKnown, sample.charging, shown, batteryDisplay.known());
    }
  }
  return batteryDisplayPercent.load();
#endif
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
