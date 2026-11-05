#include <esp_log.h>
#include <nvs.h>

#include <atomic>

#include "PlusBatteryEstimator.h"

namespace plus_board {
namespace {
BatteryEstimator estimator;
std::atomic_flag busy = ATOMIC_FLAG_INIT;
bool loaded = false;
BatteryCheckpoint persisted;
uint32_t lastAttempt = 0;
bool attempted = false;

bool readCheckpoint(BatteryCheckpoint& saved) {
  nvs_handle_t handle;
  if (nvs_open("plus-battery", NVS_READONLY, &handle) != ESP_OK) return false;
  size_t size = sizeof(saved);
  const esp_err_t result = nvs_get_blob(handle, "estimate-v1", &saved, &size);
  nvs_close(handle);
  return result == ESP_OK && size == sizeof(saved);
}

bool writeCheckpoint(const BatteryCheckpoint& saved) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open("plus-battery", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "estimate-v1", &saved, sizeof(saved));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) ESP_LOGW("PlusBattery", "checkpoint write failed: %s", esp_err_to_name(result));
  return result == ESP_OK;
}
}  // namespace

BatteryEstimate estimateBattery(const BatteryTelemetry& sample, uint32_t now) {
  if (busy.test_and_set(std::memory_order_acquire)) return {};
  // No allocation by the estimator. Only NVS may use its existing internal buffers;
  // it avoids an SD dependency and does not touch gauge/charger configuration.
  if (!loaded) {
    loaded = true;
    BatteryCheckpoint saved;
    if (readCheckpoint(saved) && estimator.restore(saved)) {
      persisted = saved;
      ESP_LOGI("PlusBattery", "restored charge checkpoint: remaining=%lu/10mAh counter=%u",
               static_cast<unsigned long>(saved.remainingDeciMah), saved.counter);
    }
  }
  const auto result = estimator.update(sample, now);
  const auto saved = estimator.checkpoint();
  const uint32_t difference = saved.remainingDeciMah > persisted.remainingDeciMah
                                  ? saved.remainingDeciMah - persisted.remainingDeciMah
                                  : persisted.remainingDeciMah - saved.remainingDeciMah;
  // First anchor/invalidation, >=5% net change, or >=10 min with >=1 mAh change.
  // Failed writes retry at most every 30 s; no writes on paints or failed samples.
  const bool dirty = saved.version != persisted.version || difference >= BATTERY_REFERENCE_DECI_MAH / 20 ||
                     (difference >= 10 && uint32_t(now - lastAttempt) >= 600000);
  if (result.known && dirty && (!attempted || uint32_t(now - lastAttempt) >= 30000)) {
    attempted = true;
    lastAttempt = now;
    if (writeCheckpoint(saved)) persisted = saved;
  }
  busy.clear(std::memory_order_release);
  return result;
}
}  // namespace plus_board
