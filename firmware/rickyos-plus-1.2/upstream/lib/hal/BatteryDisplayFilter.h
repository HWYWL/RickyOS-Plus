#pragma once

#include <cstdint>

// Display policy only: never changes battery protection, PMU/gauge configuration,
// or the raw diagnostic sample. No heap, worker task, persistence or sleep timer.
class BatteryDisplayFilter {
 public:
  static constexpr uint32_t POLL_MS = 5000;

  struct Sample {
    uint16_t percent = 0;
    uint8_t source = 0;  // 0 unknown, 1 reported SoC, 2 voltage estimate
    bool valid = false;
    bool chargingKnown = false;
    bool charging = false;
  };

  bool shouldPoll(uint32_t now) const { return !polled || uint32_t(now - lastPoll) >= POLL_MS; }
  bool known() const { return valid; }
  uint16_t value() const { return shown; }

  uint16_t update(const Sample& sample, uint32_t now) {
    if (!shouldPoll(now)) return shown;
    const bool gap = polled && uint32_t(now - lastPoll) > 120000;
    polled = true;
    lastPoll = now;
    if (!sample.valid || sample.source == 0 || sample.percent > 100) {
      resetEvidence();
      return shown;  // Transport failure is not an empty battery.
    }
    if (!valid) {
      valid = true;
      source = sample.source;
      shown = sample.percent;
      changedAt = now;
    }
    const int8_t mode = sample.chargingKnown ? (sample.charging ? 1 : 0) : -1;
    if (gap || mode != chargeMode) {
      resetEvidence();
      changedAt = now;  // Do not accumulate a smoothing budget during sleep.
      if (mode != chargeMode) {
        chargeMode = mode;
        modeChangedAt = now;
      }
    }

    // Never mix PMU SoC and voltage fallback samples in a filter window.
    if (sample.source != source) {
      if (pendingSource != sample.source || distance(sample.percent, pendingValue) > 2) {
        resetEvidence();
        pendingSource = sample.source;
        pendingValue = sample.percent;
        evidenceAt = now;
      }
      if (++evidenceCount < 3 || uint32_t(now - evidenceAt) < 10000) return shown;
      source = sample.source;
      resetEvidence();
      // A sustained low reading must not wait for a cosmetic rate limit.
      if (sample.percent <= 5) {
        shown = sample.percent;
        changedAt = now;
      }
    } else if (pendingSource != 0) {
      resetEvidence();  // An alternating source never gains confirmation.
    }

    // Two independently polled low samples bypass normal rate/settling limits.
    if (sample.percent <= 5) {
      if (++lowCount >= 2) {
        shown = sample.percent;
        changedAt = now;
        resetEvidence();
      }
      return shown;
    }
    lowCount = 0;
    samples[next] = sample.percent;
    next = (next + 1) % 3;
    if (count < 3) ++count;
    if (count < 3) return shown;
    const uint16_t a = samples[0], b = samples[1], c = samples[2];
    const uint16_t target = a < b ? (b < c ? b : (a < c ? c : a)) : (a < c ? a : (b < c ? c : b));
    if (target == shown) {
      evidenceCount = 0;
      return shown;
    }
    // Let plug/unplug voltage settle. Unknown charge state is not 'unplugged'.
    if (uint32_t(now - modeChangedAt) < 30000) return shown;
    const bool reversed = (chargeMode == 0 && target > shown) || (chargeMode == 1 && target < shown);
    if (distance(target, shown) > 5 || reversed) {
      if (evidenceCount == 0 || distance(target, pendingValue) > 2) {
        evidenceAt = now;
        pendingValue = target;
        evidenceCount = 1;
        return shown;
      }
      ++evidenceCount;
      // Repeated real corrections are allowed; direction is not locked forever.
      if (evidenceCount < 3 || uint32_t(now - evidenceAt) < (reversed ? 120000u : 30000u)) return shown;
      shown = target;
      changedAt = now;
      resetEvidence();
    } else {
      evidenceCount = 0;
      if (uint32_t(now - changedAt) >= 15000) {
        shown = target > shown ? shown + 1 : shown - 1;
        changedAt = now;
      }
    }
    return shown;
  }

 private:
  static uint16_t distance(uint16_t a, uint16_t b) { return a > b ? a - b : b - a; }
  void resetEvidence() {
    next = count = lowCount = evidenceCount = pendingSource = 0;
  }
  uint32_t lastPoll = 0, changedAt = 0, modeChangedAt = 0, evidenceAt = 0;
  uint16_t shown = 0, pendingValue = 0, samples[3]{};
  uint8_t source = 0, pendingSource = 0, next = 0, count = 0, lowCount = 0, evidenceCount = 0;
  int8_t chargeMode = -1;
  bool valid = false, polled = false;
};
