#pragma once

#include <cstdint>

// Shared time helpers used by Standby and its faces.
namespace standby_time {

constexpr uint32_t kLightSleepIdleMs = 35000u;
// After a wake that was neither the key nor the timer, sleep again this soon instead
// of waiting kLightSleepIdleMs. The PMU raises BATTERY_SAMPLE_READY (0x11) every 30 s
// and has no command to mask it; treated as input, it kept Standby awake most of
// the time. The pause only bounds the loop if the line keeps changing.
constexpr uint32_t kResleepAfterOtherWakeMs = 500u;

bool isSynced();

bool shouldLightSleep(uint32_t idleMs, bool syncActive, bool wifiActive, bool usbConnected, bool hardwareAllowed);

// Use the trustworthy wall clock when available. Before sync, tick forward
// from a plausible fallback time anchored at fallbackStartMs.
void getNowHHMM(uint32_t fallbackStartMs, unsigned& hh, unsigned& mm);

uint32_t getMinuteTick(uint32_t fallbackStartMs);

uint8_t getTenMinuteBucket(uint32_t fallbackStartMs);

bool didTenMinuteBucketChange(int16_t previousBucket, uint8_t currentBucket);

}  // namespace standby_time
