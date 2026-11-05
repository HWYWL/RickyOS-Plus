#pragma once

#include "PlusBattery.h"

namespace plus_board {
// Source: manufacturer 424860-2000 workbook, Record!A1500:L2174,
// SHA256 45d5698e685162faa8ff470ed0f434163b2c09d7be7af812b7f7b48396ffd291.
// Cycle 2 / step 6 ONLY: 1000 mA discharge to 3.3 V, 1870.7 mAh extracted.
// These are LOADED voltages, not an OCV table. No charge settings are derived here.
constexpr uint32_t BATTERY_REFERENCE_DECI_MAH = 18707;
constexpr uint16_t BATTERY_NOMINAL_MAH = 2000;
struct BatteryCurvePoint {
  uint16_t mv;
  uint16_t permille;
};
// 1%-capacity resampling, linear interpolation in measured discharged mAh,
// rounded to the nearest mV. 404 bytes of read-only Flash; no runtime allocation.
inline constexpr BatteryCurvePoint BATTERY_DISCHARGE_CURVE[] = {
    {3300, 0},   {3335, 10},  {3363, 20},  {3386, 30},  {3404, 40},  {3421, 50},  {3435, 60},  {3448, 70},  {3459, 80},
    {3471, 90},  {3481, 100}, {3491, 110}, {3501, 120}, {3510, 130}, {3519, 140}, {3527, 150}, {3535, 160}, {3543, 170},
    {3550, 180}, {3558, 190}, {3565, 200}, {3571, 210}, {3578, 220}, {3584, 230}, {3591, 240}, {3597, 250}, {3603, 260},
    {3609, 270}, {3614, 280}, {3619, 290}, {3624, 300}, {3629, 310}, {3633, 320}, {3637, 330}, {3641, 340}, {3645, 350},
    {3648, 360}, {3652, 370}, {3656, 380}, {3659, 390}, {3662, 400}, {3666, 410}, {3669, 420}, {3673, 430}, {3676, 440},
    {3680, 450}, {3684, 460}, {3689, 470}, {3693, 480}, {3698, 490}, {3702, 500}, {3707, 510}, {3712, 520}, {3717, 530},
    {3722, 540}, {3728, 550}, {3733, 560}, {3738, 570}, {3745, 580}, {3751, 590}, {3757, 600}, {3763, 610}, {3769, 620},
    {3776, 630}, {3783, 640}, {3789, 650}, {3797, 660}, {3804, 670}, {3812, 680}, {3819, 690}, {3827, 700}, {3836, 710},
    {3845, 720}, {3854, 730}, {3862, 740}, {3872, 750}, {3881, 760}, {3891, 770}, {3901, 780}, {3910, 790}, {3919, 800},
    {3929, 810}, {3939, 820}, {3948, 830}, {3957, 840}, {3968, 850}, {3978, 860}, {3988, 870}, {3998, 880}, {4009, 890},
    {4020, 900}, {4031, 910}, {4042, 920}, {4055, 930}, {4067, 940}, {4079, 950}, {4094, 960}, {4109, 970}, {4127, 980},
    {4152, 990}, {4211, 1000}};

constexpr uint16_t batteryLoadedPermille(uint16_t mv) {
  if (mv <= BATTERY_DISCHARGE_CURVE[0].mv) return 0;
  for (unsigned i = 1; i < sizeof(BATTERY_DISCHARGE_CURVE) / sizeof(BATTERY_DISCHARGE_CURVE[0]); ++i) {
    const auto& hi = BATTERY_DISCHARGE_CURVE[i];
    const auto& lo = BATTERY_DISCHARGE_CURVE[i - 1];
    if (mv <= hi.mv) {
      const uint32_t span = hi.mv - lo.mv;
      return lo.permille + ((mv - lo.mv) * uint32_t(hi.permille - lo.permille) + span / 2) / span;
    }
  }
  return 1000;
}

struct BatteryTelemetry {
  uint16_t mv = 0;
  int16_t current = 0;  // BQ27220: positive = charge, negative = discharge, mA.
  int16_t averageCurrent = 0;
  uint16_t counter = 0;  // RawCoulombCount 0x22: mAh, increases on discharge.
  bool currentKnown = false;
  bool averageKnown = false;
  bool counterKnown = false;
  bool gaugeReady = false;
};

enum class BatteryEstimateSource : uint8_t { UncalibratedVoltage, LoadedCurve, CoulombCount };
struct BatteryEstimate {
  uint16_t percent = 0;
  BatteryEstimateSource source = BatteryEstimateSource::UncalibratedVoltage;
  bool known = false;
};

// Explicit fields and checksum; no pointers, packed/unaligned access or ABI-dependent bools.
struct BatteryCheckpoint {
  uint32_t version = 0;
  uint32_t remainingDeciMah = 0;
  uint16_t counter = 0;
  uint16_t reserved = 0;
  uint32_t checksum = 0;
};

constexpr uint32_t batteryCheckpointChecksum(const BatteryCheckpoint& saved) {
  uint32_t crc = 0xffffffffu;
  const uint32_t fields[] = {saved.version, saved.remainingDeciMah,
                             uint32_t(saved.counter) | (uint32_t(saved.reserved) << 16)};
  for (auto field : fields) {
    for (unsigned byte = 0; byte < 4; ++byte) {
      crc ^= (field >> (byte * 8)) & 255u;
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
  }
  return ~crc;
}

// Physical estimate, deliberately separate from the presentation filter.
// Fixed-size state; no timers/tasks, no gauge writes, no integration of the last
// instantaneous current across long gaps. The gauge counts charge while the MCU sleeps.
class BatteryEstimator {
 public:
  bool restore(const BatteryCheckpoint& saved) {
    if (saved.version != 1 || saved.reserved != 0 || saved.remainingDeciMah > BATTERY_REFERENCE_DECI_MAH ||
        saved.checksum != batteryCheckpointChecksum(saved))
      return false;
    remaining = saved.remainingDeciMah;
    previousCounter = saved.counter;
    anchored = true;
    timed = false;
    return true;
  }

  BatteryCheckpoint checkpoint() const {
    BatteryCheckpoint saved;
    if (anchored) {
      saved.version = 1;
      saved.remainingDeciMah = remaining;
      saved.counter = previousCounter;
    }
    saved.checksum = batteryCheckpointChecksum(saved);
    return saved;
  }

  BatteryEstimate update(const BatteryTelemetry& sample, uint32_t now) {
    BatteryEstimate result;
    uint16_t fallback = 0;
    if (!batteryPercentFromMv(sample.mv, fallback)) {
      resetEvidence();
      motionTimed = false;
      return result;
    }
    result.percent = fallback;
    result.known = true;
    if (!sample.gaugeReady || !sample.counterKnown) {
      resetEvidence();
      motionTimed = false;
      // Once calibrated, an I2C error must not switch the UI to a different model.
      if (anchored) result.known = false;
      return result;
    }

    // Read success is not proof that RCC actually counts on this gauge/firmware.
    // Check liveness only over short, observed discharge intervals. This integral
    // detects a frozen counter; it NEVER supplies the capacity estimate itself.
    if (motionTimed && sample.counter == motionCounter && uint32_t(now - motionTime) <= 90000 && sample.currentKnown &&
        sample.averageKnown && sample.current < -5 && sample.averageCurrent < -5) {
      stationaryMaMs += int64_t(-sample.averageCurrent) * uint32_t(now - motionTime);
      if (stationaryMaMs >= 4LL * 3600000) counterFault = true;
    } else {
      stationaryMaMs = 0;
      if (motionTimed && sample.counter != motionCounter) counterFault = false;
    }
    motionTime = now;
    motionCounter = sample.counter;
    motionTimed = true;
    if (counterFault) {
      anchored = false;
      timed = false;
      resetEvidence();
      return result;
    }

    if (anchored) {
      const uint16_t modular = uint16_t(sample.counter - previousCounter);
      const int32_t delta = modular <= 32767 ? int32_t(modular) : int32_t(modular) - 65536;
      const uint32_t elapsed = now - previousTime;
      // 2 A is a corruption guard, not a charge-current setting. Allow 2 mAh of
      // quantisation/read timing slack. At restore, accept at most one pack's net change.
      const uint32_t limit = timed ? uint32_t(uint64_t(elapsed) * 2000 / 3600000) + 2 : 1871;
      const uint32_t magnitude = delta < 0 ? uint32_t(-delta) : uint32_t(delta);
      // TI clears RCC on FC. It is NOT a measured negative delta. Also reject
      // backwards resets while discharging; do not credit fictitious charge.
      const bool reset = (sample.counter == 0 && previousCounter > 2 && previousCounter < 65534) ||
                         (delta < -2 && sample.currentKnown && sample.averageKnown && sample.current < -5 &&
                          sample.averageCurrent < -5);
      if (magnitude > limit || reset) {
        anchored = false;
        resetEvidence();
      } else {
        int32_t next = int32_t(remaining) - delta * 10;
        remaining = next < 0                                     ? 0
                    : next > int32_t(BATTERY_REFERENCE_DECI_MAH) ? BATTERY_REFERENCE_DECI_MAH
                                                                 : uint32_t(next);
      }
    }
    previousCounter = sample.counter;
    previousTime = now;
    timed = true;

    const bool currentsValid = sample.currentKnown && sample.averageKnown;
    // Manufacturer charge ended at 4296.8 mV / 39.6 mA. Repeated high-voltage,
    // low-taper observations establish a full reference, not voltage alone/FC alone.
    const bool full = currentsValid && sample.mv >= 4290 && sample.current >= -5 && sample.current <= 40 &&
                      sample.averageCurrent >= -5 && sample.averageCurrent <= 40;
    const bool referenceLoad = currentsValid && sample.current >= -1050 && sample.current <= -950 &&
                               sample.averageCurrent >= -1050 && sample.averageCurrent <= -950;
    const bool low = currentsValid && sample.mv <= 3300 && sample.current < -5 && sample.averageCurrent < -5;
    if (qualify(fullEvidence, full, now, 180000)) {
      remaining = BATTERY_REFERENCE_DECI_MAH;
      anchored = true;
    } else if (qualify(lowEvidence, low, now, 30000)) {
      remaining = 0;
      anchored = true;
    } else if (!anchored && qualify(loadEvidence, referenceLoad, now, 30000)) {
      remaining = (uint32_t(batteryLoadedPermille(sample.mv)) * BATTERY_REFERENCE_DECI_MAH + 500) / 1000;
      anchored = true;
      result.source = BatteryEstimateSource::LoadedCurve;
    }
    // Keep all evidence clocks up to date even when an earlier branch wins.
    if (!full) fullEvidence = {};
    if (!low) lowEvidence = {};
    if (!referenceLoad) loadEvidence = {};
    if (anchored) {
      result.percent = (remaining * 100u + BATTERY_REFERENCE_DECI_MAH / 2) / BATTERY_REFERENCE_DECI_MAH;
      if (result.source != BatteryEstimateSource::LoadedCurve) result.source = BatteryEstimateSource::CoulombCount;
    }
    return result;
  }

 private:
  struct Evidence {
    uint32_t start = 0;
    uint32_t last = 0;
    uint16_t samples = 0;
  };
  static bool qualify(Evidence& evidence, bool condition, uint32_t now, uint32_t duration) {
    if (!condition) {
      evidence = {};
      return false;
    }
    if (!evidence.samples || uint32_t(now - evidence.last) > 90000) {
      evidence = {now, now, 1};
      return false;
    }
    // Repainting repeatedly at the same instant is not new evidence.
    if (uint32_t(now - evidence.last) >= 5000) {
      evidence.last = now;
      if (evidence.samples < 65535) ++evidence.samples;
    }
    return evidence.samples >= 4 && uint32_t(now - evidence.start) >= duration;
  }
  void resetEvidence() {
    fullEvidence = {};
    lowEvidence = {};
    loadEvidence = {};
  }
  uint32_t remaining = 0;
  uint32_t previousTime = 0;
  uint16_t previousCounter = 0;
  bool anchored = false;
  bool timed = false;
  int64_t stationaryMaMs = 0;
  uint32_t motionTime = 0;
  uint16_t motionCounter = 0;
  bool motionTimed = false;
  bool counterFault = false;
  Evidence fullEvidence;
  Evidence lowEvidence;
  Evidence loadEvidence;
};

// Board owner: serialises the estimator and stores bounded checkpoints in NVS.
BatteryEstimate estimateBattery(const BatteryTelemetry& sample, uint32_t now);
}  // namespace plus_board
