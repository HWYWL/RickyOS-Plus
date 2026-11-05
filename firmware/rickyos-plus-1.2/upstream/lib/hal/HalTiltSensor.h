#pragma once

#include <Arduino.h>
#include <Imu.h>

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

// Knock twice on the back to turn the page (SC7A20H click detector).
namespace CrossPointBackTap {
enum Value : uint8_t { TAP_OFF = 0, TAP_NEXT = 1, TAP_PREVIOUS = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  mutable Imu _sdkImu;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _inTilt = false;            // Currently tilted past threshold
  bool _isAwake = false;           // Tracks power state
  unsigned long _initMs = 0;       // Timestamp of sensor init
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown
  unsigned long _wakeMs = 0;       // Timestamp of last wake() for stabilization

  // Tuning constants
  static constexpr float RATE_THRESHOLD_DPS = 270.0f;  // Deg/sec speed to trigger flick
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;     // Must stop moving below this rate before next trigger
  // Boards whose IMU has no gyroscope (SC7A20H, Read Pico) drive the same gesture from
  // the accelerometer's gravity component. Rotating about the tilt axis gives
  // a = g*sin(theta), so da/dt = g*cos(theta)*omega and at small angles da/dt = g*omega.
  // Dividing by g cancels it, so 1 dps == pi/180 g/s and the gyro thresholds convert
  // exactly: 270 dps -> 4.71 g/s, 50 dps -> 0.87 g/s. Same flick, same feel.
  static constexpr float DPS_TO_GPS = 0.0174533f;
  static constexpr float RATE_THRESHOLD_GPS = RATE_THRESHOLD_DPS * DPS_TO_GPS;
  static constexpr float NEUTRAL_RATE_GPS = NEUTRAL_RATE_DPS * DPS_TO_GPS;
  static constexpr unsigned long COOLDOWN_MS = 600;            // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;        // 20 Hz polling
  static constexpr unsigned long ACCEL_POLL_INTERVAL_MS = 80;  // SC7A20H samples at 12.5 Hz
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;      // Ignore readings after wake

  mutable unsigned long _lastPollMs = 0;

  // Back double tap: the IMU's own detector, polled with the tilt. A finger on the
  // screen shakes the same axis, so taps shortly after a touch are dropped.
  static constexpr unsigned long TAP_POLL_INTERVAL_MS = 60;
  static constexpr unsigned long TAP_AFTER_TOUCH_MS = 600;
  static constexpr unsigned long TAP_COOLDOWN_MS = 500;
  uint8_t _tapMode = CrossPointBackTap::TAP_OFF;
  uint8_t _tapSensitivity = 1;
  bool _tapOn = false;  // detector running in the IMU
  bool _tapNextEvent = false;
  bool _tapPrevEvent = false;
  unsigned long _touchMs = 0;
  unsigned long _lastTapMs = 0;
  unsigned long _lastTapPollMs = 0;
  // Software detector over the 200 Hz FIFO samples of the axis through the screen: a
  // slow baseline removes gravity, a knock is a short spike above the threshold, and two
  // knocks 40..500 ms apart are a double tap. Times count samples (200 Hz: 5 ms each).
  // Bench (Plus, knuckle on the back): first knocks 0.7..1.3 g, second ones often only
  // 0.2..0.3 g, many lasting 35..95 ms; nothing at rest above 0.13 g.
  static constexpr float TAP_THRESHOLDS_G[3] = {0.35f, 0.20f, 0.13f};  // firm, medium, light
  static constexpr uint32_t TAP_MAX_SAMPLES = 20;                      // a knock ends within 100 ms
  static constexpr uint32_t TAP_GAP_MIN = 8, TAP_GAP_MAX = 100;       // 40..500 ms between knocks
  float _tapBase = 0.0f;
  bool _tapBaseValid = false;
  bool _tapSpike = false;
  float _tapSpikePeak = 0.0f;
  uint32_t _tapSample = 0, _tapSpikeStart = 0, _lastKnock = 0;
  void syncTap(bool wanted);
  void pollTaps(unsigned long now);
  void onDoubleTap(unsigned long now, const char* how);

  // Set when the active IMU is an accelerometer with no gyroscope, so the tilt axis is
  // differentiated from gravity instead of read as an angular rate.
  bool _accelOnly = false;
  float _lastTiltG = 0.0f;  // Previous gravity-component sample, for the rate
  unsigned long _lastTiltGMs = 0;
  CrossPointTiltPageTurn::Value _accelMode = CrossPointTiltPageTurn::TILT_OFF;
  CrossPointOrientation::Value _accelOrientation = CrossPointOrientation::PORTRAIT;
  bool _accelReading = false;

  bool readGyro(float& gx, float& gy, float& gz) const;
  bool readAccel(float& ax, float& ay, float& az) const;

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // Enables tilt polling state
  bool wake();

  // Puts tilt polling state to sleep
  bool deepSleep();

  // True if an IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll the accelerometer and update tilt and back-tap gesture state.
  void update(const uint8_t mode, const uint8_t orientation, const bool inReader,
              const uint8_t tapMode = CrossPointBackTap::TAP_OFF, const uint8_t tapSensitivity = 1);

  // The screen was touched: hold back-tap detection for a moment (see TAP_AFTER_TOUCH_MS).
  void noteTouch() { _touchMs = millis(); }

  // Consumed once per back double tap, by the page direction it was set to.
  bool wasBackTappedNext();
  bool wasBackTappedPrevious();

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events (call when leaving reader or disabling tilt).
  void clearPendingEvents();
};
