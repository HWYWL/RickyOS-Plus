#include "HalTiltSensor.h"

#include <BoardConfig.h>
#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

namespace {

// Map the sensor's in-plane axes to the forward/back tilt convention, which is the
// gyro's: portrait tilts about X and uses `primary`, landscape about Y and uses
// `secondary`, and TILT_INVERTED flips the sign. Accelerometer boards pass the
// perpendicular axis as `primary`/`secondary` (see the call site).
float tiltAxisFor(const uint8_t mode, const uint8_t orientation, const float primary, const float secondary) {
  const bool inverted = mode == CrossPointTiltPageTurn::TILT_INVERTED;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      return inverted ? -primary : primary;
    case CrossPointOrientation::INVERTED:
      return inverted ? primary : -primary;
    case CrossPointOrientation::LANDSCAPE_CW:
      return inverted ? secondary : -secondary;
    case CrossPointOrientation::LANDSCAPE_CCW:
      return inverted ? -secondary : secondary;
    default:
      return primary;
  }
}

}  // namespace

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  gx = sample.gx;
  gy = sample.gy;
  gz = sample.gz;
  return true;
}

bool HalTiltSensor::readAccel(float& ax, float& ay, float& az) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  ax = sample.ax;
  ay = sample.ay;
  az = sample.az;
  return true;
}

void HalTiltSensor::begin() {
  _available = _sdkImu.begin();
  if (!_available) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }

  // The SC7A20H (Read Pico) is a 3-axis accelerometer with no gyroscope: read() fills
  // ax/ay/az but reports gx = gy = gz = 0, so the angular-rate gesture below could never
  // fire. The same flick is still measurable -- see RATE_THRESHOLD_GPS in the header --
  // by differentiating the gravity component, so the part is brought up like any other
  // and update() takes the accelerometer path. isAvailable() then reports true, which is
  // the gate SettingsList uses to offer "Tilt page turn".
  // Read Pico and Metalio Plus both carry the SC7A20H.
  _accelOnly = BoardConfig::ACTIVE.sensors.imuType == BoardConfig::ImuType::Sc7a20h;

  _initMs = millis();
  _lastPollMs = millis();
  // begin() leaves the sensors sampling; stand them by until tilt page turn actually
  // wakes them, so a disabled IMU doesn't drain the battery.
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU standby failed");
  }
  LOG_INF("GYR", "SDK IMU initialized (%s)", _accelOnly ? "accelerometer only, gravity rate" : "gyroscope rate");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  if (_accelOnly) {
    _lastTiltGMs = 0;
    _inTilt = false;
    clearPendingEvents();
  }
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  _tapOn = false;
  _sdkImu.disableDoubleTap();  // off with the sensor; wake() comes back at the tilt rate
  return true;
}

void HalTiltSensor::syncTap(const bool wanted) {
  if (wanted == _tapOn) return;
  _tapOn = wanted ? _sdkImu.enableDoubleTap(_tapSensitivity) : !_sdkImu.disableDoubleTap();
  _tapNextEvent = _tapPrevEvent = false;
  _tapBaseValid = _tapSpike = false;
  _lastKnock = 0;
  if (wanted && !_tapOn) LOG_ERR("GYR", "double tap unavailable");
  else LOG_INF("GYR", "double tap %s (strength %u)", _tapOn ? "on" : "off", static_cast<unsigned>(_tapSensitivity));
  uint8_t regs[8];
  if (_tapOn && _sdkImu.readTapRegisters(regs))
    LOG_INF("GYR", "tap regs ctrl1=%02x ctrl2=%02x ctrl5=%02x fifo=%02x cfg=%02x ths=%02x lim=%02x win=%02x", regs[0],
            regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]);
}

void HalTiltSensor::onDoubleTap(const unsigned long now, const char* how) {
  if (now - _touchMs < TAP_AFTER_TOUCH_MS || now - _lastTapMs < TAP_COOLDOWN_MS) {
    LOG_INF("GYR", "double tap (%s) ignored: touch or cooldown", how);
    return;
  }
  _lastTapMs = now;
  _lastTiltMs = now;  // the knock must not also read as a tilt flick
  _hadActivity = true;
  (_tapMode == CrossPointBackTap::TAP_PREVIOUS ? _tapPrevEvent : _tapNextEvent) = true;
  LOG_INF("GYR", "Back double tap (%s, %s)", how, _tapMode == CrossPointBackTap::TAP_PREVIOUS ? "previous" : "next");
}

void HalTiltSensor::pollTaps(const unsigned long now) {
  uint8_t source = 0;
  if (_sdkImu.takeDoubleTap(&source)) onDoubleTap(now, "chip");
  else if (source) LOG_INF("GYR", "click src=0x%02x (not a double tap)", source);
  float z[32];
  bool lost = false;
  const int n = _sdkImu.readTapAxis(z, 32, lost);
  if (n < 0) return;
  if (lost) {  // a stall (a long render) dropped samples: their timing is gone
    _tapSpike = false;
    _lastKnock = 0;
  }
  const float threshold = TAP_THRESHOLDS_G[_tapSensitivity < 3 ? _tapSensitivity : 1];
  for (int i = 0; i < n; ++i) {
    ++_tapSample;
    if (!_tapBaseValid) {
      _tapBase = z[i];
      _tapBaseValid = true;
      continue;
    }
    const float level = fabsf(z[i] - _tapBase);
    if (!_tapSpike) {
      _tapBase += (z[i] - _tapBase) * 0.04f;  // ~125 ms: follows gravity as the hand moves
      if (level > threshold) {
        _tapSpike = true;
        _tapSpikeStart = _tapSample;
        _tapSpikePeak = level;
      }
      continue;
    }
    if (level > _tapSpikePeak) _tapSpikePeak = level;
    if (level >= threshold * 0.5f) {
      if (_tapSample - _tapSpikeStart > TAP_MAX_SAMPLES * 3) {  // a sustained push or shake, not a knock
        // Usually the device was turned: gravity moved. Restart the baseline here, or it
        // would stay at the old angle and every later sample would read as this push.
        _tapSpike = false;
        _lastKnock = 0;
        _tapBase = z[i];
      }
      continue;
    }
    _tapSpike = false;
    const uint32_t duration = _tapSample - _tapSpikeStart;
    LOG_INF("GYR", "knock peak=%.2fg %ums", _tapSpikePeak, static_cast<unsigned>(duration * 5));
    if (duration > TAP_MAX_SAMPLES) {
      _lastKnock = 0;
      continue;
    }
    const uint32_t gap = _lastKnock ? _tapSpikeStart - _lastKnock : 0;
    if (_lastKnock && gap >= TAP_GAP_MIN && gap <= TAP_GAP_MAX) {
      _lastKnock = 0;
      onDoubleTap(now, "knock");
    } else {
      _lastKnock = _tapSpikeStart;
    }
  }
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader,
                           const uint8_t tapMode, const uint8_t tapSensitivity) {
  if (!_available) {
    return;
  }

  const bool tapWanted = tapMode != CrossPointBackTap::TAP_OFF && inReader;
  if (tapSensitivity != _tapSensitivity && _tapOn) {
    syncTap(false);  // re-armed below with the new threshold
  }
  _tapSensitivity = tapSensitivity;
  _tapMode = tapMode;
  const bool shouldBeAwake = (mode != CrossPointTiltPageTurn::TILT_OFF || tapWanted) && inReader;
  if (_accelOnly && (mode != _accelMode || orientation != _accelOrientation || shouldBeAwake != _accelReading)) {
    _accelMode = static_cast<CrossPointTiltPageTurn::Value>(mode);
    _accelOrientation = static_cast<CrossPointOrientation::Value>(orientation);
    _accelReading = shouldBeAwake;
    _lastTiltGMs = 0;
    _inTilt = false;
    clearPendingEvents();
  }
  if (shouldBeAwake && !_isAwake) {
    _isAwake = wake();
    return;
  }
  if (!shouldBeAwake && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  if (!shouldBeAwake) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  syncTap(tapWanted);
  if (_tapOn && now - _lastTapPollMs >= TAP_POLL_INTERVAL_MS) {
    _lastTapPollMs = now;
    pollTaps(now);
  }
  if (mode == CrossPointTiltPageTurn::TILT_OFF) {
    return;
  }

  const unsigned long pollInterval = _accelOnly ? ACCEL_POLL_INTERVAL_MS : POLL_INTERVAL_MS;
  if ((now - _lastPollMs) < pollInterval) {
    return;
  }
  _lastPollMs = now;

  // Gyro boards read an angular rate directly; accelerometer boards differentiate the
  // gravity component, so the two arrive here in different units and against thresholds
  // converted from the same 270 dps. See RATE_THRESHOLD_GPS in the header.
  float tiltAxis;
  float rateThreshold;
  float neutralRate;
  if (_accelOnly) {
    float ax, ay, az;
    if (!readAccel(ax, ay, az)) {
      _lastTiltGMs = 0;
      return;
    }
    // The gyro measures angular rate ABOUT the tilt axis; the accelerometer measures the
    // gravity component ALONG the axis perpendicular to it. The gesture is a LEFT/RIGHT
    // sway, so in portrait the device rolls about its up/down axis and gravity swings
    // along the left/right axis (ax); landscape swaps which axis is which, so it swings
    // along ay. Mapping the gyro axis straight across would fire on the wrong motion.
    //
    // The sign is flipped because the accelerometer's gravity swing runs opposite to the
    // gyro's rotation rate for the same physical sway; flipping here rather than inside
    // tiltAxisFor() leaves the gyro boards' convention exactly as it was.
    const float sample = -tiltAxisFor(mode, orientation, ax, ay);
    if (_lastTiltGMs == 0) {
      _lastTiltG = sample;
      _lastTiltGMs = now;
      return;
    }
    tiltAxis = (sample - _lastTiltG) * 1000.0f / static_cast<float>(now - _lastTiltGMs);
    _lastTiltG = sample;
    _lastTiltGMs = now;
    rateThreshold = RATE_THRESHOLD_GPS;
    neutralRate = NEUTRAL_RATE_GPS;
  } else {
    float gx, gy, gz;
    if (!readGyro(gx, gy, gz)) {
      return;
    }
    // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
    tiltAxis = tiltAxisFor(mode, orientation, gx, gy);
    rateThreshold = RATE_THRESHOLD_DPS;
    neutralRate = NEUTRAL_RATE_DPS;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < neutralRate) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > rateThreshold) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) %s", tiltAxis, _accelOnly ? "g/s" : "dps");
      } else if (tiltAxis < -rateThreshold) {
        _tiltBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) %s", tiltAxis, _accelOnly ? "g/s" : "dps");
      }
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::wasBackTappedNext() {
  const bool val = _tapNextEvent;
  _tapNextEvent = false;
  return val;
}

bool HalTiltSensor::wasBackTappedPrevious() {
  const bool val = _tapPrevEvent;
  _tapPrevEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _tapNextEvent = false;
  _tapPrevEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
