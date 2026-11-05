#pragma once

// FreeInk inertial measurement unit (LSM6DS3TR-C or QMI8658, 6-axis accel +
// gyro; SC7A20H, 3-axis accelerometer only).
//
// Reads acceleration (g) and angular rate (deg/s) from the I2C IMU described by
// BoardConfig::ACTIVE.sensors (imuAddr / sensor bus). Dependency-free Wire
// access, mirroring BatteryMonitor. Boards without an IMU (FREEINK_CAP_IMU off,
// or imuAddr == 0) link stub bodies and present() returns false.
//
// ImuType::Sc7a20h (Read Pico) has NO gyroscope: read() still succeeds and still
// fills ax/ay/az, but gx/gy/gz are always 0. The Sample shape is deliberately
// unchanged, so a consumer that derives a gesture from angular rate (e.g.
// lib/hal/HalTiltSensor.cpp, whose tilt page turn needs ~270 dps) simply never
// fires on such a board — see docs/engineering/read-pico.md for that gap.

#include <Arduino.h>

#include <cstdint>

namespace freeink {

class Imu {
 public:
  struct Sample {
    float ax, ay, az;  // acceleration, g (1 g ~= 9.81 m/s^2)
    float gx, gy, gz;  // angular rate, degrees/second
  };

  // Verifies WHO_AM_I and configures accel + gyro for the active board.
  // Returns false when the active board has no IMU or the part doesn't identify.
  bool begin();
  bool present() const { return begun_; }

  // Reads one accel + gyro sample. Returns false on I2C error.
  bool read(Sample& out);

  // Puts the sensors into hardware standby / power-down. Config registers are
  // retained, so wake() restores sampling without a full begin(). Returns
  // false when the IMU is absent or on I2C error.
  bool sleep();

  // Restarts sampling after sleep(). Returns false when absent or on I2C
  // error; allow for a settling transient before trusting samples.
  bool wake();

  // SC7A20H only: the part's own double-tap (click) detector on the axis through the
  // screen, so a knock on the back is one gesture. Polled through its latched source
  // register, no interrupt line. Raises sampling to 200 Hz while on (sleep() still
  // powers it down; wake() restores it). sensitivity: 0 firm, 1 medium, 2 light.
  bool enableDoubleTap(uint8_t sensitivity);
  bool disableDoubleTap();
  // True once per detected double tap; reading clears the latch. `source`, when given,
  // receives the raw click source (single taps included) for tuning.
  bool takeDoubleTap(uint8_t* source = nullptr);
  // While double tap is on, the 200 Hz samples also queue in the part's 32-entry FIFO, so
  // a caller polling every few dozen ms sees every knock. Reads up to `max` queued
  // samples of the axis through the screen, in g; returns the count (-1 on I2C error).
  // `overflowed` reports samples lost since the last read.
  int readTapAxis(float* out, int max, bool& overflowed);
  // Diagnostic: CTRL1, CTRL2, CTRL5, FIFO_CTRL, CLICK_CFG, CLICK_THS, TIME_LIMIT, TIME_WINDOW.
  bool readTapRegisters(uint8_t (&out)[8]);

 private:
  bool begun_ = false;
  // The QMI8658 can legally appear at 0x6A or 0x6B depending on its SA0
  // strap. Keep the address found by begin() instead of repeatedly using the
  // board profile's preferred address.
  uint8_t addr_ = 0;
  bool doubleTap_ = false;
};

}  // namespace freeink

using Imu = freeink::Imu;
