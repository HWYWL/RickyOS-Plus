#pragma once

#include <cstdint>

// FreeInk SDK — deep-sleep / wake power management.
//
// Owns the one hardware concern the rest of the SDK leaves to the consumer: the
// per-SoC deep-sleep GPIO-wakeup difference. RISC-V parts (C3/C6/H2) wake from
// deep sleep via the "gpio" source (esp_deep_sleep_enable_gpio_wakeup); Xtensa
// parts (S3/S2, classic ESP32) wake via RTC ext1 (esp_sleep_enable_ext1_wakeup).
// Hardcoding either one blocks a multi-MCU build (see docs/consumer-mcu-portability.md).
//
// This picks the right source at compile time from SoC capability macros, and
// reads the wake pin + active level from BoardConfig::ACTIVE.input, so the same
// consumer code deep-sleeps correctly on every supported board. The wake pin must
// be RTC-capable on ext1 parts (true for the de-link power button).

namespace freeink {

class PowerManager {
 public:
  // Arm wake-on-power-button using the SoC-correct wakeup source and the active
  // board's power pin + polarity (powerActiveHigh -> wake on HIGH, else LOW).
  // Returns false if the board has no power pin (PIN_UNASSIGNED); nothing armed.
  static bool armPowerButtonWakeup();

  // Arm deep-sleep wake on an arbitrary set of GPIOs (gpioMask, wakeLow = wake on
  // the low level) using the SoC-correct source (ext1 on Xtensa, gpio on RISC-V).
  // Use for extra wake lines beyond the power button — a touch INT, a second
  // button, an IO-expander INT. The pins must be RTC-capable on ext1 parts:
  // check each one with isDeepSleepWakePin() before adding it to the mask. On
  // ESP32-S3 the RTC-capable pads are GPIO0..21, so a line like the Read Pico
  // FCA9555 INT# on GPIO41 can never be an EXT1 source — it has no RTC pad at
  // all — and neither can the CST836U INT# on GPIO43; both are light-sleep-only
  // (armLightSleepWakeup below, read-pico.md B9).
  static void armWakeOnPins(uint64_t gpioMask, bool wakeLow = true);

  // True when `pin` can wake the chip from DEEP sleep: an RTC-capable pad on
  // ext1 parts (Xtensa), any valid pin on the RISC-V parts that use the
  // deep-sleep "gpio" source. Does not touch any hardware state.
  static bool isDeepSleepWakePin(int8_t pin);

  // Arm GPIO level wake for LIGHT sleep: each pin in `gpioMask` wakes the chip
  // when it holds the selected level, and the caller then runs
  // esp_light_sleep_start(). Light sleep accepts every digital pin, RTC-capable
  // or not, which is the only way to wake on a line like the Read Pico FCA9555
  // INT# (GPIO41) or the SC7A20H INT1 (GPIO1). Returns false if a pin could not
  // be armed or the source could not be enabled; nothing is left half-armed.
  static bool armLightSleepWakeup(uint64_t gpioMask, bool wakeLow = true);

  // Same, with an independent level per pin — Read Pico arms the expander INT#
  // on the low level and, when pickup-to-wake is enabled, the accelerometer INT1
  // on the high level (read_pico_firmware main/sleep.c). Separate name rather
  // than an overload so `armLightSleepWakeup(mask, 0)` can never be ambiguous
  // between a bool and a second mask.
  static bool armLightSleepWakeupLevels(uint64_t lowMask, uint64_t highMask);

  // Disarm the GPIO light-sleep source: clears the level trigger of every pin in
  // `gpioMask` and disables the shared ESP_SLEEP_WAKEUP_GPIO source. Call it
  // after esp_light_sleep_start() returns; safe when nothing was armed.
  static void clearLightSleepWakeup(uint64_t gpioMask);

  // Poll the power-button GPIO (raw read, with the matching pull) until released,
  // so deep sleep isn't immediately cancelled by a still-held press.
  static void waitForPowerButtonRelease();

  // Drive every assigned peripheral power-rail enable in the active board
  // profile (display / SD / touch / mic) to its OFF level and latch it with
  // gpio_hold_en() so the load switches stay off through deep sleep (deepSleep()
  // enables gpio_deep_sleep_hold_en(), which makes the holds persist). Without
  // this, boards with gated rails (e.g. Sticky: GT911 on TP_PWR_EN, SD on
  // SD_PWR_EN, EPD on EP_PWR_EN) leave those peripherals powered all through
  // deep sleep — milliamps of standby drain. No-op on boards whose rails are
  // PIN_UNASSIGNED (X4/X3). Call after the display driver's deep-sleep command
  // and before deepSleep(); wake is a chip reset, so rails re-enable in the
  // normal init path. Display RESET is held LOW when its rail is cut (avoids
  // back-powering an unpowered controller) and HIGH when its rail remains on
  // (keeps deep-sleep state stable). NOTE: cutting the touch rail forfeits
  // touch-to-wake.
  static void powerDownRailsForSleep();

  // Isolate floating GPIOs to cut sleep current, then enter deep sleep. Does not
  // return — the chip resets on wake.
  //
  // Boards whose "off" is a PMU-driven host shutdown rather than an ESP deep
  // sleep install a host-shutdown hook (setHostShutdownHook). When one is set,
  // this runs THAT instead of esp_deep_sleep_start(): such a board exposes no
  // ESP-side deep-sleep wake source, so arming one would leave the chip asleep
  // with nothing able to wake it (Read Pico: the CW32L010 owns the host EN rail
  // and the real wake sources are the PMU key, AC-in and the RTC alarm —
  // read-pico.md B9/B10). If the hook returns, the PMU never cut the rail, so
  // the call idles here rather than falling through to a wake-less deep sleep.
  [[noreturn]] static void deepSleep();

  // Convenience: wait for release, arm the power-button wakeup, then deep sleep.
  [[noreturn]] static void deepSleepUntilPowerButton();

  // Board-owned "turn the device off" implementation, used in place of
  // esp_deep_sleep_start() by deepSleep() / deepSleepUntilPowerButton(). It may
  // return: the caller then treats the shutdown as failed and idles. The board
  // registers it from its own support layer, so this class stays device-agnostic
  // (same pattern as SDCardManager::setPowerHook / InputManager::setButtonHook).
  // Default: none — every other target keeps the ESP deep-sleep path unchanged.
  using HostShutdownHook = void (*)();
  static void setHostShutdownHook(HostShutdownHook hook);
  // esp_deep_sleep_start() is documented as not returning, but deepSleep()
  // calls esp_restart() if it ever does (instead of a busy-loop -- see that
  // function). This reports whether the *previous* boot took that abort
  // path, and clears the record so it only surfaces once. wakeupCause/
  // wakePinLevel are the values captured at the moment of the abort.
  struct AbortedSleepInfo {
    bool aborted = false;
    int wakeupCause = 0;
    int wakePinLevel = 0;
  };
  static AbortedSleepInfo takeAbortedSleepInfo();
};

}  // namespace freeink
