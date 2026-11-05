#include "PlusBoard.h"
#include "PlusAudio.h"
#include "PlusGaugeConfig.h"
#include "PlusSleepBreadcrumb.h"
#if defined(RICKYOS_PLUS_STATIC_STANDBY) && defined(RICKYOS_STANDBY_DIAG)
#include <esp_attr.h>
#include <esp_system.h>
#endif
#include <InputManager.h>
#include <PowerManager.h>
#include <plus_i2c_wire.h>
#include <metalio_i2c1.h>
#include <epd_board_metalio_eink4_plus.h>
#include <config.h>
#include <esp_log.h>
#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <driver/usb_serial_jtag.h>
#include <hal/usb_serial_jtag_ll.h>
#include <esp_sleep.h>
#include <esp_private/mspi_timing_tuning.h>
#include <metalio_touch.h>
#include <StandbyDiag.h>
#include <lcd_fault.h>
#if defined(RICKYOS_PLUS_STATIC_STANDBY)
#include <metalio_sd.h>
#endif
#if defined(RICKYOS_PLUS_IDLE_FREQ_MHZ) && defined(RICKYOS_STANDBY_DIAG)
#include <BoardConfig.h>
#include <CrossPointSettings.h>
#endif
namespace plus_board {
namespace {
using SleepStage = plus_sleep_trace::Stage;
#if defined(RICKYOS_PLUS_STATIC_STANDBY) && defined(RICKYOS_STANDBY_DIAG)
RTC_NOINIT_ATTR volatile plus_sleep_trace::Record sleepTrace[2];
void sleepCheckpoint(SleepStage stage) { plus_sleep_trace::checkpoint(sleepTrace, stage, millis()); }
void recordSleepWake(unsigned cause, int error) {
  plus_sleep_trace::checkpoint(sleepTrace, SleepStage::SleepReturned, millis(), cause,
                               static_cast<uint32_t>(error), cause == ESP_SLEEP_WAKEUP_TIMER);
}
void reportSleepReset() {
  const auto saved = plus_sleep_trace::latest(sleepTrace);
  const auto& w = saved.record.words;
  const uint32_t last = saved.valid && w[5] ? w[6 + w[5] - 1] : 0;
  STANDBY_DIAG("reset reason=%u trace=%u last=%lu seq=%lu cycles=%lu t=%lu", unsigned(esp_reset_reason()),
               saved.valid, (unsigned long)last, (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4]);
  if (saved.valid) STANDBY_DIAG("reset trail=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", (unsigned long)w[6],
      (unsigned long)w[7], (unsigned long)w[8], (unsigned long)w[9], (unsigned long)w[10],
      (unsigned long)w[11], (unsigned long)w[12], (unsigned long)w[13]);
  if (saved.valid) STANDBY_DIAG("reset wake cause=%lu err=%ld timers=%lu", (unsigned long)w[14],
                              (long)static_cast<int32_t>(w[15]), (unsigned long)w[16]);
  sleepCheckpoint(SleepStage::Boot);
}
#else
void sleepCheckpoint(SleepStage) {}
void recordSleepWake(unsigned, int) {}
void reportSleepReset() {}
#endif
// On by default since 1.2.6 (CMake RICKYOS_PLUS_DEFAULT_ON): the panel recovers
// after wake once MSPI high-speed timing is restored. Diagnostics alone never
// opt a device into sleep.
#if defined(RICKYOS_PLUS_STANDBY_SLEEP) && RICKYOS_PLUS_STANDBY_SLEEP == 1
constexpr bool kStandbySleepEnabled = true;
#else
constexpr bool kStandbySleepEnabled = false;
#endif
bool boardReady = false;
bool sleepFault = false;
// Static standby: one park lasts up to a day (the caller passes the time left
// to auto power-off); at most this many shared-IRQ wakes re-sleep in place.
constexpr uint32_t kStaticSleepMaxSeconds = 24u * 60u * 60u;
constexpr unsigned kStaticMaxOtherWakes = 2000;
constexpr uint8_t kPowerKeyMask = 0x80;
constexpr uint8_t kAllKeyMask = 0x83;

bool readInputs(uint8_t& p0, uint8_t& p1) {
  // Each board accessor owns the non-recursive shared bus mutex internally.
  return epd_board_metalio_eink4_plus_ioexp_read_port(0, &p0) &&
         epd_board_metalio_eink4_plus_ioexp_read_port(1, &p1);
}

bool standbyPowerIdle() {
  // Adapter classification can be zero while a PC is attached (observed on
  // hardware). SOF detection independently protects the native USB connection.
  // Neither a zero classification nor a zero charge state proves cable absence;
  // a power-only adapter may still supply the board after charge termination.
  if (usb_serial_jtag_is_connected()) return false;
  uint8_t status = 0;
  if (!metalio_i2c1_lock(20)) return false;
  const auto error = plus_i2c_read(0x6b, 0x1e, &status, 1);
  metalio_i2c1_unlock();
  return error == ESP_OK && (status & 0x1f) == 0 && !usb_serial_jtag_is_connected();
}

void restoreUsbDebugAfterSleep() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  // OEM ReviveUsjAfterSdRestore: restore USJ clocks/pads and force a bounded
  // disconnect so a host can enumerate again. Static standby remounts SD before
  // this step; the USB-MSC routing switch remains unchanged.
  usb_serial_jtag_ll_enable_bus_clock(true);
  usb_serial_jtag_ll_phy_enable_pad(false);
  delay(150);
  usb_serial_jtag_ll_phy_set_defaults();
  usb_serial_jtag_ll_phy_enable_pad(true);
  // No wait for SOF: an unplugged device is a valid state, not a restore error.
#endif
}

bool touchIdle() {
  // Plus polls the FT6336U point frame, not its IRQ level. GPIO5 can stay low
  // without a contact. Accept only a successful zero-contact frame; malformed
  // frames, held cover keys and bus errors remain blockers. Stack storage only.
  uint8_t frame[5]{};
  if (!metalio_i2c1_lock(20)) return false;
  const auto error = plus_i2c_read(TOUCH_I2C_ADDR, 0x02, frame, sizeof(frame));
  metalio_i2c1_unlock();
  return error == ESP_OK && (frame[0] & 0x0f) == 0;
}

#if defined(RICKYOS_PLUS_STATIC_STANDBY)
// OEM sc7a20h.cc WriteSilenceRegs: low-active open-drain, all IRQ routes
// disabled, 1 Hz with axes off. Save/restore the polling configuration on stack.
struct StandbyImu {
  static constexpr uint8_t regs[] = {0x20, 0x22, 0x24, 0x25, 0x30};
  static constexpr uint8_t silence[] = {0x10, 0x00, 0x00, 0x03, 0x00};
  uint8_t saved[5]{};
  uint8_t address = 0;
  bool touched = false;
  bool park() {
    if (!metalio_i2c1_lock(20)) return false;
    for (uint8_t candidate : {uint8_t(SC7A20H_I2C_ADDR_DEFAULT), uint8_t(SC7A20H_I2C_ADDR_ALT)}) {
      uint8_t who = 0;
      if (plus_i2c_read(candidate, 0x0f, &who, 1) == ESP_OK && who == 0x11) { address = candidate; break; }
    }
    bool ok = address != 0;
    for (unsigned i = 0; ok && i < 5; ++i) ok = plus_i2c_read(address, regs[i], &saved[i], 1) == ESP_OK;
    if (ok) {
      touched = true;
      for (unsigned i : {4u, 1u, 2u, 0u, 3u}) {
        if (plus_i2c_write(address, regs[i], &silence[i], 1) != ESP_OK) ok = false;
      }
      uint8_t source = 0;
      if (plus_i2c_read(address, 0x31, &source, 1) != ESP_OK) ok = false;
      for (unsigned i = 0; i < 5; ++i) {
        uint8_t actual = 0xff;
        if (plus_i2c_read(address, regs[i], &actual, 1) != ESP_OK || actual != silence[i]) ok = false;
      }
    }
    metalio_i2c1_unlock();
    return ok;
  }
  bool restore() {
    if (!touched) return true;
    if (!metalio_i2c1_lock(20)) return false;
    bool ok = true;
    for (unsigned i : {4u, 1u, 2u, 3u, 0u}) {
      if (plus_i2c_write(address, regs[i], &saved[i], 1) != ESP_OK) ok = false;
    }
    for (unsigned i = 0; i < 5; ++i) {
      uint8_t actual = 0xff;
      if (plus_i2c_read(address, regs[i], &actual, 1) != ESP_OK || actual != saved[i]) ok = false;
    }
    metalio_i2c1_unlock();
    return ok;
  }
};
#endif

bool inputsIdle(uint8_t p1, bool rtcInput = false, bool checkIrq = true) {
  const int irqLevel = rtcInput ? rtc_gpio_get_level(IO_EXPANDER_INT_GPIO) : gpio_get_level(IO_EXPANDER_INT_GPIO);
  return (p1 & kAllKeyMask) == kAllKeyMask && (!checkIrq || irqLevel != 0) &&
         gpio_get_level(BOOT_BUTTON_GPIO) != BOOT_BUTTON_ACTIVE_LEVEL && touchIdle();
}

StandbyWake observePowerRelease() {
  // A confirmed low P17 after wake is a real press. Give short presses time to
  // finish; still-held presses belong to main.cpp's ordinary long-press owner.
  const uint32_t started = millis();
  uint32_t releasedAt = 0;
  bool releasing = false;
  bool down = true;
  while (uint32_t(millis() - started) < 250) {
    delay(10);
    uint8_t p0 = 0, p1 = 0;
    if (!readInputs(p0, p1)) return StandbyWake::Failed;
    if ((p1 & 3) != 3) return StandbyWake::Failed;
    down = (p1 & kPowerKeyMask) == 0;
    if (down) {
      releasing = false;
    } else if (!releasing) {
      releasedAt = millis();
      releasing = true;
    } else if (uint32_t(millis() - releasedAt) >= 40) {
      return StandbyWake::PowerButton;
    }
  }
  return down ? StandbyWake::PowerButtonHeld : StandbyWake::Failed;
}

// A side key woke static standby: leave Standby like a power-key short press,
// after the key is released so the next screen does not also see it as a page
// turn. Stack only; bounded to 2 s (a held key then belongs to the input owner).
StandbyWake observeSideKeyRelease() {
  const uint32_t started = millis();
  while (uint32_t(millis() - started) < 2000) {
    uint8_t p0 = 0, p1 = 0;
    if (!readInputs(p0, p1)) return StandbyWake::Failed;
    if ((p1 & 3) == 3) return StandbyWake::PowerButton;
    delay(10);
  }
  return StandbyWake::PowerButtonHeld;
}

uint8_t buttons() {
  uint8_t value = 0xff;
  if (!metalio_i2c1_lock(20)) return 0;
  const auto result = plus_i2c_read(0x20, 0x01, &value, 1);
  metalio_i2c1_unlock();
  if (result != ESP_OK) return 0;
  uint8_t keys = 0;
  if (!(value & 0x80)) keys |= 1u << InputManager::BTN_POWER;
  if (!(value & 0x01)) keys |= 1u << InputManager::BTN_UP;
  if (!(value & 0x02)) keys |= 1u << InputManager::BTN_DOWN;
  return keys;
}
void shutdown() {
  plus_audio::shutdown();
  // Match the OEM PwrOffTask: a returning pulse train means the CPU is still
  // powered, not that shutdown succeeded. Keep requesting hardware power-off
  // instead of returning to the SDK's indefinite idle fallback. The board
  // routine preserves the OEM pulse waveform; no charger/BATFET writes here.
  ESP_LOGW("PlusBoard", "requesting hardware power-off until the rail is cut");
  for (;;) {
    epd_board_metalio_eink4_plus_pwr_key_pulse_train();
    delay(200);
  }
}
}
// CX25601N charger at 0x6B: the OEM firmware's boot configuration, ported from
// CloudZao/Metalio-E-INK4-Plus main/boards/common/power/cx25601n.cc (hw_init_defaults,
// set_vreg/ichg/iindpm; MIT, Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co.,
// Ltd. and Project Contributors). The chip keeps these while the battery stays
// connected, so boards that came from the OEM firmware still have them; one that lost
// battery power fell back to the chip defaults and charged slower. Each field is a
// read-modify-write of exactly the OEM's bits; 960 mA charge and input limit, 4.4 V.
bool configureCharger() {
  constexpr uint8_t kAddr = 0x6b;
  if (!metalio_i2c1_lock(50)) return false;
  bool ok = plus_i2c_probe(kAddr) == ESP_OK;
  const auto write = [&](uint8_t reg, uint8_t value) {
    if (ok) ok = plus_i2c_write(kAddr, reg, &value, 1) == ESP_OK;
  };
  const auto field = [&](uint8_t reg, uint8_t mask, uint8_t shift, uint8_t value) {
    uint8_t cur = 0;
    if (ok) ok = plus_i2c_read(kAddr, reg, &cur, 1) == ESP_OK;
    const uint8_t m = static_cast<uint8_t>(mask << shift);
    write(reg, static_cast<uint8_t>((cur & ~m) | ((value << shift) & m)));
  };
  field(0x15, 0x01, 6, 0);     // D+/D- detection off: the input limit below applies
  field(0x16, 0x01, 4, 0);     // HIZ off
  field(0x16, 0x03, 0, 0);     // watchdog off, so the settings stay
  field(0x10, 0x0f, 4, 0x0c);  // precharge current
  field(0x11, 0x01, 0, 0);
  field(0x12, 0x1f, 3, 0x06);  // termination current
  field(0x13, 0x01, 0, 0);
  field(0x04, 0x1f, 3, 440 & 0x1f);  // VREG 4400 mV (10 mV steps)
  field(0x05, 0x0f, 0, 440 >> 5);
  field(0x02, 0x03, 6, 12 & 0x03);   // ICHG 960 mA (80 mA steps)
  field(0x03, 0x0f, 0, 12 >> 2);
  field(0x06, 0x0f, 4, 48 & 0x0f);   // IINDPM 960 mA (20 mA steps)
  field(0x07, 0x0f, 0, 48 >> 4);
  if (ok) {
    // OEM private registers, behind its unlock sequence (0x70 reads 0x03 when open).
    bool unlocked = false;
    for (int i = 0; i < 10 && ok && !unlocked; ++i) {
      for (const uint8_t key : {0x00, 0x50, 0x57, 0x44}) write(0x70, key);
      uint8_t v = 0;
      unlocked = ok && plus_i2c_read(kAddr, 0x70, &v, 1) == ESP_OK && v == 0x03;
    }
    if (unlocked) {
      write(0x86, 0x06);
      write(0x3a, 0x10);
      write(0x46, 0x20);
    }
    write(0x70, 0x00);
    if (!unlocked) ESP_LOGW("PlusCharger", "private register unlock failed");
  }
  field(0x14, 0x01, 0, 1);
  field(0x18, 0x01, 2, 0);
  field(0x1a, 0x01, 7, 1);
  field(0x23, 0x07, 2, 0x07);
  field(0x24, 0x01, 3, 1);
  field(0x16, 0x01, 5, 1);     // charging enabled
  metalio_i2c1_unlock();
  if (ok) ESP_LOGI("PlusCharger", "configured: 960 mA charge/input, 4400 mV, watchdog off");
  else ESP_LOGW("PlusCharger", "configuration failed; chip settings unchanged or partial");
  return ok;
}

bool begin() {
  reportSleepReset();
  // The keys, rails and touch all sit behind the TCA on this bus. A reset that lands mid-
  // transaction (esptool, a brown-out) has left it unanswering for one boot, and then no
  // key worked until the next restart: try again before giving up. Both steps are
  // idempotent (Wire1 starts once; the expander is probed, then configured).
  bool bus = false, io = false;
  for (int attempt = 0; attempt < 3 && !(bus && io); ++attempt) {
    if (attempt) delay(20);
    bus = plus_i2c_begin() == ESP_OK;
    io = bus && epd_board_metalio_eink4_plus_safe_io_init();
    if (!(bus && io)) ESP_LOGE("PlusBoard", "init attempt %d failed (bus=%d io=%d)", attempt + 1, bus, io);
  }
  if (!bus || !io) return false;
  epd_board_metalio_eink4_plus_tp_reset();
  InputManager::setButtonHook(buttons);
  freeink::PowerManager::setHostShutdownHook(shutdown);
#if defined(RICKYOS_PLUS_GAUGE_CONFIG)
  configureGauge();  // pack values into the gauge's RAM config; once per pack connection
#endif
  configureCharger();
  boardReady = true;
  return true;
}

bool idleCpuFrequencyAllowed() { return !plus_audio::busy(); }

void traceIdlePower() {
#if defined(RICKYOS_PLUS_IDLE_FREQ_MHZ) && defined(RICKYOS_STANDBY_DIAG)
  STANDBY_DIAG("idle cfg: face=%u off=%u off_s=%lu sleep=%u", SETTINGS.rickyStandbyFace,
               SETTINGS.rickyPowerOffScreen, SETTINGS.getStandbyPowerOffMs() / 1000UL, kStandbySleepEnabled);
  const uint8_t addr = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
  if (addr == 0 || !metalio_i2c1_lock(20)) {
    STANDBY_DIAG("idlepower: cpu=%u read_error=1", getCpuFrequencyMhz());
    return;
  }
  // Same read-only BQ registers as BatteryMonitor. Four stack words only;
  // neither the gauge nor the charger is configured by this diagnostic.
  constexpr uint8_t regs[] = {0x08, 0x0c, 0x14, 0x22};
  uint16_t values[4]{};
  bool ok = true;
  for (unsigned i = 0; i < 4; ++i) {
    uint8_t raw[2]{};
    if (plus_i2c_read(addr, regs[i], raw, sizeof(raw)) != ESP_OK) {
      ok = false;
      break;
    }
    values[i] = uint16_t(raw[0]) | (uint16_t(raw[1]) << 8);
  }
  metalio_i2c1_unlock();
  if (!ok) {
    STANDBY_DIAG("idlepower: cpu=%u read_error=1", getCpuFrequencyMhz());
    return;
  }
  STANDBY_DIAG("idlepower: cpu=%u mv=%u i=%d avg=%d q=%u", getCpuFrequencyMhz(), values[0],
               static_cast<int16_t>(values[1]), static_cast<int16_t>(values[2]), values[3]);
#endif
}

bool standbySleepReady() {
  // A restored allocation can succeed while the first real frame underruns.
  // Preserve that first fault and remain awake instead of repeating the same
  // failed sleep/refresh cycle (observed at row 426 after the first timer wake).
  if (kStandbySleepEnabled && !sleepFault && epd_lcd_get_first_fault() != 0) {
    sleepFault = true;
    ESP_LOGE("PlusSleep", "display first fault=0x%08lx; sleep disabled until restart",
             static_cast<unsigned long>(epd_lcd_get_first_fault()));
  }
#ifdef RICKYOS_STANDBY_DIAG
  static uint32_t lastReadyNote = 0;
  if (uint32_t(millis() - lastReadyNote) >= 30000) {
    lastReadyNote = millis();
    STANDBY_DIAG("board ready=%u fault=%u audio=%u touch_pending=%u", boardReady, sleepFault,
                 plus_audio::busy(), metalio_touch_has_pending_edges());
  }
#endif
  return kStandbySleepEnabled && boardReady && !sleepFault && !plus_audio::busy() && !metalio_touch_has_pending_edges();
}

static StandbyWake standbySleepOnce(const uint32_t seconds, StandbyStorage* storage) {
  const bool staticPark = storage != nullptr;
  const auto skip = [](const char* reason) {
#ifdef RICKYOS_STANDBY_DIAG
    static const char* previousReason = nullptr;
    static uint32_t lastNote = 0;
    if (reason != previousReason || uint32_t(millis() - lastNote) >= 30000) {
      previousReason = reason;
      lastNote = millis();
      STANDBY_DIAG("board skip: %s", reason);
    }
#else
    (void)reason;
#endif
    return StandbyWake::Failed;
  };
  if (!standbySleepReady()) return skip("not ready/audio/touch pending");
  // Static standby parks once and sleeps until the power key or the auto-off
  // deadline (up to a day); the clock-style path keeps its one-minute slices.
  if (seconds == 0 || seconds > (staticPark ? kStaticSleepMaxSeconds : 60u)) return skip("timer range");
  if (!standbyPowerIdle()) return skip("USB/charger active or read failed");
  uint8_t p0 = 0, p1 = 0;
  if (!readInputs(p0, p1)) return skip("expander read failed");
  if (!inputsIdle(p1, false, !staticPark)) {
    STANDBY_DIAG("inputs p1=%02x irq=%d touch=%d boot=%d", p1, gpio_get_level(IO_EXPANDER_INT_GPIO),
                 gpio_get_level(TOUCH_INT_PIN), gpio_get_level(BOOT_BUTTON_GPIO));
    return skip("input active");
  }
  gpio_io_config_t config{};
  if (gpio_get_io_config(IO_EXPANDER_INT_GPIO, &config) != ESP_OK || !config.ie || config.oe)
    return skip("GPIO2 not input");
  if (!esp_sleep_is_valid_wakeup_gpio(IO_EXPANDER_INT_GPIO)) return skip("GPIO2 not EXT1 capable");

  // Match the OEM's low-level EXT1 wake without changing the normal digital
  // ISR type. Take ownership of the RTC input explicitly: IDF's EXT1 prepare
  // selects the RTC mux and may hold the pad, but disabling EXT1 does not undo
  // either. Keep the CPU ISR masked until the pad is digital again.
  if (gpio_intr_disable(IO_EXPANDER_INT_GPIO) != ESP_OK) return StandbyWake::Failed;
  bool rtcTouched = false;
  bool sourceTouched = false;
  bool timerTouched = false;
  const auto cleanup = [&] {
    esp_err_t firstError = ESP_OK;
    const auto keepError = [&](esp_err_t error) {
      if (firstError == ESP_OK && error != ESP_OK) firstError = error;
    };
    // Remove only our pin, preserving unrelated EXT1 configuration. Release
    // the sleep hold before rtc_gpio_deinit returns the pin to digital control
    // and clears the RTC input/pulls/clock ownership acquired by rtc_gpio_init.
    if (sourceTouched) keepError(esp_sleep_disable_ext1_wakeup_io(1ULL << IO_EXPANDER_INT_GPIO));
    if (timerTouched) keepError(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER));
    if (rtcTouched) {
      keepError(rtc_gpio_hold_dis(IO_EXPANDER_INT_GPIO));
      keepError(rtc_gpio_deinit(IO_EXPANDER_INT_GPIO));
    }
    keepError(gpio_intr_enable(IO_EXPANDER_INT_GPIO));
    if (firstError != ESP_OK) {
      sleepFault = true;
      ESP_LOGE("PlusSleep", "wake cleanup failed: %s; sleep disabled until restart", esp_err_to_name(firstError));
    }
    return firstError == ESP_OK;
  };

  rtcTouched = true;
  esp_err_t error = rtc_gpio_init(IO_EXPANDER_INT_GPIO);
  if (error == ESP_OK) error = rtc_gpio_set_direction(IO_EXPANDER_INT_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  // S31 has separate digital and RTC pull registers. Mirror the existing
  // electrical configuration, without changing the digital sleep selection.
  if (error == ESP_OK)
    error = config.pu ? rtc_gpio_pullup_en(IO_EXPANDER_INT_GPIO) : rtc_gpio_pullup_dis(IO_EXPANDER_INT_GPIO);
  if (error == ESP_OK)
    error = config.pd ? rtc_gpio_pulldown_en(IO_EXPANDER_INT_GPIO) : rtc_gpio_pulldown_dis(IO_EXPANDER_INT_GPIO);
  if (error == ESP_OK) {
    sourceTouched = true;
    error = esp_sleep_enable_ext1_wakeup_io(1ULL << IO_EXPANDER_INT_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  if (error == ESP_OK) {
    timerTouched = true;
    error = esp_sleep_enable_timer_wakeup(uint64_t(seconds) * 1000000ULL);
  }
  if (error != ESP_OK) {
    cleanup();
    ESP_LOGW("PlusSleep", "wake setup failed: %s", esp_err_to_name(error));
    return StandbyWake::Failed;
  }

  // Recheck after arming to close the read/configure race. Never hold the shared
  // I2C mutex across sleep, change expander directions, or cut peripheral rails.
  if (!standbyPowerIdle() || !readInputs(p0, p1)) {
    cleanup();
    return StandbyWake::Failed;
  }
  if ((p1 & kPowerKeyMask) == 0) {
    if (!cleanup()) return StandbyWake::Failed;
    if ((p1 & 3) != 3) return StandbyWake::Failed;
    return observePowerRelease();
  }
  if (!inputsIdle(p1, true, !staticPark)) {
    cleanup();
    return StandbyWake::Failed;
  }

#if defined(RICKYOS_PLUS_STATIC_STANDBY)
  StandbyImu imu;
  bool storageStopped = false, sdPadsTouched = false, keysTouched = false, railsTouched = false;
  [[maybe_unused]] bool p5Touched = false;
  const auto restorePeripherals = [&] {
    bool ok = true;
    bool keysRestored = true;
    if (keysTouched) {
      sleepCheckpoint(SleepStage::KeysRestore);
      keysRestored = epd_board_metalio_eink4_plus_ioexp_restore_keys_checked();
    }
    bool railsRestored = true;
    if (railsTouched) {
      sleepCheckpoint(SleepStage::RailsOn);
      railsRestored = epd_board_metalio_eink4_plus_sleep_restore_rails_staged(keysRestored, [](unsigned phase) {
        if (phase < 6) sleepCheckpoint(static_cast<SleepStage>(static_cast<uint32_t>(SleepStage::RailP14Write) + phase));
      });
    }
#if defined(RICKYOS_PLUS_STANDBY_CUT_P5)
    if (p5Touched && keysRestored) {
      sleepCheckpoint(SleepStage::RailP5Write);
      railsRestored = epd_board_metalio_eink4_plus_sleep_restore_p5() && railsRestored;
      sleepCheckpoint(SleepStage::RailP5Waited);
    }
#endif
    const bool ioRestored = keysRestored && (!keysTouched || epd_board_metalio_eink4_plus_sleep_io_checked(false));
    sleepCheckpoint(SleepStage::SdRelease);
#if defined(RICKYOS_PLUS_SD_FAULT_TEST)
    // Test build only: the first wake leaves the SD pads held low, so the
    // remount fails as on the reported card; the pads are released right after.
    static bool sdFaultInjected = false;
    const bool injectSdFault = sdPadsTouched && !sdFaultInjected;
    sdFaultInjected = sdFaultInjected || injectSdFault;
    const bool padsRestored = !sdPadsTouched || injectSdFault || MetalioSd_ResumeBusChecked();
#else
    const bool padsRestored = !sdPadsTouched || MetalioSd_ResumeBusChecked();
#endif
    if (!railsRestored || !ioRestored || !padsRestored) ok = false;
    // Remount through the SAME SDK manager, then restore every readonly position
    // before USJ revives. The legacy VFS MetalioSd_Init is deliberately not used.
    sleepCheckpoint(SleepStage::SdMount);
    // A card that does not mount here is not a board fault: HalStorage keeps
    // its files parked, mounts it again on the next storage use, and stops
    // parking it in later Standbys. Leaving it out of `ok` keeps the wake key
    // and later sleeps working instead of disabling sleep until restart.
    const bool sdRestored = !storageStopped || storage->resume(storage->context);
#if defined(RICKYOS_PLUS_SD_FAULT_TEST)
    if (injectSdFault) MetalioSd_ResumeBusChecked();
#endif
    sleepCheckpoint(SleepStage::ImuRestore);
    const bool imuRestored = imu.restore();
    if (!imuRestored) ok = false;
    if (!ok) sleepFault = true;
    STANDBY_DIAG("static restore: keys=%u rails=%u io=%u pads=%u sd=%u imu=%u ok=%u", keysRestored, railsRestored, ioRestored, padsRestored,
                 sdRestored, imuRestored, ok);
    return ok;
  };
  if (staticPark) {
    sleepCheckpoint(SleepStage::SdStop);
    if (!storage->suspend(storage->context)) { cleanup(); return skip("storage busy/unsupported"); }
    storageStopped = true;
    sdPadsTouched = true;
    sleepCheckpoint(SleepStage::SdPads);
    const bool sdParked = MetalioSd_ParkBusChecked();
    sleepCheckpoint(SleepStage::TpsSleep);
    epd_board_metalio_eink4_plus_tps_enter_sleep();
    // Bus resources are released below, once, before the key/rail handoff.
    sleepCheckpoint(SleepStage::BusPark);
    epd_board_metalio_eink4_plus_bus_hold_low();
    sleepCheckpoint(SleepStage::ImuPark);
    const bool imuParked = imu.park();
    keysTouched = true;
    sleepCheckpoint(SleepStage::KeysMask);
    epd_board_metalio_eink4_plus_ioexp_mask_for_sleep();
    sleepCheckpoint(SleepStage::PowerInput);
    const bool powerArmed = epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin();
    uint8_t check0 = 0, check1 = 0;
    const bool inputsRead = readInputs(check0, check1);
    const int irqLevel = rtc_gpio_get_level(IO_EXPANDER_INT_GPIO);
    bool parked = sdParked && imuParked && powerArmed && inputsRead &&
                  (check1 & kPowerKeyMask) != 0 && irqLevel != 0;
    int ioChecked = -1;
    if (parked) {
#if !defined(RICKYOS_PLUS_STANDBY_KEEP_RAILS)
      railsTouched = true;
      sleepCheckpoint(SleepStage::RailsOff);
      epd_board_metalio_eink4_plus_sleep_cut_rails();
#elif defined(RICKYOS_PLUS_STANDBY_CUT_P5)
      // P14 stays on (the panel needs it); only P5 goes off and comes back on wake.
      p5Touched = true;
      sleepCheckpoint(SleepStage::RailsOff);
      epd_board_metalio_eink4_plus_sleep_cut_p5();
#elif defined(RICKYOS_PLUS_STANDBY_LAZY_BT)
      // P5 stays on; P14 goes off and stays off after wake (audio restores it).
      sleepCheckpoint(SleepStage::RailsOff);
      epd_board_metalio_eink4_plus_sleep_park_bt_pa();
#endif
      // KEEP_RAILS: P14/P5 stay on through sleep. Re-enabling P14 after a long
      // park reset the board with a power glitch even on the first restore
      // (park1, after ~30 min), so this variant never cuts or restores them.
      sleepCheckpoint(SleepStage::IoCheck);
      parked = epd_board_metalio_eink4_plus_sleep_io_checked(true);
      ioChecked = parked;
    }
    STANDBY_DIAG("static park: sd=%u imu=%u key=%u input=%u p1=%02x irq=%d io=%d", sdParked,
                 imuParked, powerArmed, inputsRead, check1, irqLevel, ioChecked);
    if (!parked) {
      cleanup();
      const bool peripheralOk = restorePeripherals();
      sleepCheckpoint(SleepStage::UsbRestore);
      restoreUsbDebugAfterSleep();
      sleepCheckpoint(SleepStage::LcdRestore);
      const bool displayOk = epd_board_metalio_eink4_plus_bus_restore_checked();
      if (!peripheralOk || !displayOk) sleepFault = true;
      return skip("static park failed; restored");
    }
  }
#else
  if (staticPark) { cleanup(); return StandbyWake::Failed; }
#endif

  // The custom LCD/GDMA/RMT path does not register IDF light-sleep retention.
  // Use the board's existing shallow-sleep lifecycle under the caller's render
  // lock. Deinit frees its existing DMA resources before restore reallocates
  // them; EPDiy's image/line queues remain owned by the display backend.
  // Static profile also parks SD/TPS/keys/rails above; no charger writes.
  if (!staticPark) epd_board_metalio_eink4_plus_bus_hold_low();
  sleepCheckpoint(SleepStage::BusLatch);
  epd_board_metalio_eink4_plus_bus_latch_for_sleep();
  // Standby turns the frontlight off before it sleeps (StandbyActivity never
  // sleeps while it is lit), so both YX6016N EN/PWM pads are low here. Latch
  // them: the LEDC channel stops in light sleep and must not leave the driver
  // enabled. Released below, before anything can light it again.
  gpio_hold_en(FL_COOL_PIN);
  gpio_hold_en(FL_WARM_PIN);
  ESP_LOGI("PlusSleep", "enter: seconds=%lu ext1_gpio=2", static_cast<unsigned long>(seconds));
  STANDBY_DIAG("board sleep enter: seconds=%lu display_fault=0x%08lx", static_cast<unsigned long>(seconds),
               static_cast<unsigned long>(epd_lcd_get_first_fault()));
  // Park once, sleep many: a static face needs nothing while asleep, so timer
  // slices and shared-IRQ wakes go straight back to sleep with SD, TPS, keys
  // and the P14/P5 rails still parked. Only the power key or the auto-off
  // deadline restores peripherals, once. (Restoring the rails on every slice
  // reset the board with a power glitch on the second cycle: static3/static4.)
  const uint32_t sleepStarted = millis();
  const uint32_t sleepBudgetMs = seconds * 1000u;
  decltype(esp_sleep_get_wakeup_cause()) cause{};
  bool wakeInputsValid = false;
  unsigned otherWakes = 0;
  for (;;) {
    sleepCheckpoint(SleepStage::SleepEnter);
    error = esp_light_sleep_start();
    // IDF drops MSPI (PSRAM) to its low-speed timing before sleep and restores
    // it on wake only when the CPU clock is >= 150 MHz on S31 (clk_utils.c,
    // BELOW_FREQ_THRESHOLD). Standby runs at 80 MHz, so PSRAM came back at a
    // quarter of its read rate (96 -> 22 MB/s, park3) and the first refresh's
    // row feed underran: a white screen. Return MSPI to the high-speed timing it
    // had before sleep, at this same CPU clock.
    mspi_timing_change_speed_mode_cache_safe(false);
    cause = esp_sleep_get_wakeup_cause();
    recordSleepWake(static_cast<unsigned>(cause), error);
    // As in the OEM wake sequence, latch P17 before restoring peripherals or
    // re-enabling the shared GPIO ISR: LCD reinitialization may outlast a tap.
    // Always restore the bus after a failed read; never turn that read into a key.
    sleepCheckpoint(SleepStage::WakeLatch);
    wakeInputsValid = error == ESP_OK && readInputs(p0, p1);
    STANDBY_DIAG("wake latch: valid=%u p1=%02x cause=%d other=%u", wakeInputsValid, p1, static_cast<int>(cause),
                 otherWakes);
    if (!staticPark || !wakeInputsValid || (p1 & kAllKeyMask) != kAllKeyMask) break;
    // OEM latch (397): with IMU, TPS and RTC silenced, an expander IRQ while
    // parked is a key. A quick tap is often released before this first read,
    // and re-sleeping on it made the power key look dead (park5).
    if (cause == ESP_SLEEP_WAKEUP_EXT1) break;
    const uint32_t slept = millis() - sleepStarted;
    if (cause == ESP_SLEEP_WAKEUP_TIMER || slept + 1000u >= sleepBudgetMs) break;
    // A shared-IRQ pulse (IMU, TPS, charger): the read above cleared the
    // expander's INT. Sleep again once GPIO2 is back high; a line that stays
    // low, or a storm of such wakes, ends the session awake instead of spinning.
    if (++otherWakes > kStaticMaxOtherWakes) break;
    bool released = false;
    for (unsigned i = 0; i < 10 && !released; ++i) {
      released = rtc_gpio_get_level(IO_EXPANDER_INT_GPIO) != 0;
      if (!released) delay(5);
    }
    if (!released) break;
    if (esp_sleep_enable_timer_wakeup(uint64_t(sleepBudgetMs - slept) * 1000ULL) != ESP_OK) break;
  }
  const uint8_t wakeP1 = p1;
  STANDBY_DIAG("wake final: valid=%u p1=%02x cause=%d other=%u slept_ms=%lu", wakeInputsValid, wakeP1,
               static_cast<int>(cause), otherWakes, static_cast<unsigned long>(millis() - sleepStarted));
  sleepCheckpoint(SleepStage::WakeCleanup);
  gpio_hold_dis(FL_COOL_PIN);
  gpio_hold_dis(FL_WARM_PIN);
  const bool restored = cleanup();
#if defined(RICKYOS_PLUS_STATIC_STANDBY)
  const bool peripheralRestored = !staticPark || restorePeripherals();
#else
  const bool peripheralRestored = true;
#endif
  // Restore even on a rejected/failed sleep or GPIO cleanup error, before any
  // Activity can redraw or process the key. Never leave the bus held low.
  // Keep debug recovery independent of LCD allocation, including a rejected
  // sleep attempt: clock/pad preparation may already have run on that path.
  // An unplugged device is allowed; restoration never waits for a host.
  sleepCheckpoint(SleepStage::UsbRestore);
  restoreUsbDebugAfterSleep();
  STANDBY_DIAG("USB restore done; display restore begin");
  sleepCheckpoint(SleepStage::LcdRestore);
  const bool displayRestored = epd_board_metalio_eink4_plus_bus_restore_checked();
  STANDBY_DIAG("display restore done: ok=%u", displayRestored);
  if (!displayRestored) {
    sleepFault = true;
    ESP_LOGE("PlusSleep", "LCD restore failed; sleep disabled until restart");
  }
  STANDBY_DIAG("board sleep return: error=%d cause=%d cleanup=%u", static_cast<int>(error),
               static_cast<int>(cause), restored);
  if (error != ESP_OK || !restored || !displayRestored || !peripheralRestored) {
    if (error != ESP_OK) ESP_LOGW("PlusSleep", "sleep failed: %s", esp_err_to_name(error));
    return StandbyWake::Failed;
  }
  if (!wakeInputsValid) return StandbyWake::Failed;
  ESP_LOGI("PlusSleep", "wake: cause=%d p0=0x%02x p1=0x%02x irq=%d", static_cast<int>(cause), p0, wakeP1,
           gpio_get_level(IO_EXPANDER_INT_GPIO));
  if ((wakeP1 & kPowerKeyMask) == 0) {
    if ((wakeP1 & 3) != 3) return StandbyWake::Failed;
    return observePowerRelease();
  }
  if (staticPark && (wakeP1 & 3) != 3) return observeSideKeyRelease();
  // Already released: a tap that ended before the latch read still leaves Standby.
  if (staticPark && cause == ESP_SLEEP_WAKEUP_EXT1) return StandbyWake::PowerButton;
  // GPIO2 also carries RTC/IMU/TPS events. A pulse already released before the
  // first successful read cannot be attributed to P17; never invent a key press.
  const bool deadline = staticPark && uint32_t(millis() - sleepStarted) + 1000u >= sleepBudgetMs;
  return cause == ESP_SLEEP_WAKEUP_TIMER || deadline ? StandbyWake::Timer : StandbyWake::Failed;
}
StandbyWake standbyLightSleep(const uint32_t seconds) { return standbySleepOnce(seconds, nullptr); }
StandbyWake standbyStaticSleep(const uint32_t seconds, StandbyStorage& storage) {
  const auto reason = standbySleepOnce(seconds, &storage);
  switch (reason) {
    case StandbyWake::Timer: sleepCheckpoint(SleepStage::ExitTimer); break;
    case StandbyWake::PowerButton: sleepCheckpoint(SleepStage::ExitPower); break;
    case StandbyWake::PowerButtonHeld: sleepCheckpoint(SleepStage::ExitHeld); break;
    default: sleepCheckpoint(SleepStage::ExitFailed); break;
  }
  return reason;
}
}
