#pragma once
#include "PlusNetAllocTrace.h"

#include <Logging.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdint>

// Plus radio owners may finish in a different order from acquisition (SNTP
// starts before the web server). Keep the original mode until the last owner
// leaves. The shared mutex and state use static storage, never heap memory.
class HalWifiAwakeLease {
  struct State {
    StaticSemaphore_t storage{};
    SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
    unsigned owners = 0;
    wifi_ps_type_t previous = WIFI_PS_MIN_MODEM;
#if RICKYOS_PLUS_TX_POWER_COMPAT
    int8_t previousPower = 0;
    int8_t appliedPower = 0;
#endif
  };
  static State& state() {
    static State value;
    return value;
  }
  bool held_ = false;

 public:
  HalWifiAwakeLease() = default;
  HalWifiAwakeLease(const HalWifiAwakeLease&) = delete;
  HalWifiAwakeLease& operator=(const HalWifiAwakeLease&) = delete;
  ~HalWifiAwakeLease() { release(); }

  bool acquire() {
    auto& s = state();
    if (!s.mutex || xSemaphoreTake(s.mutex, portMAX_DELAY) != pdTRUE) return false;
    if (!held_) {
      wifi_ps_type_t previous;
      const esp_err_t result = esp_wifi_get_ps(&previous);
      if (result != ESP_OK) {
        LOG_ERR("WIFI", "Failed to acquire radio-awake lease");
        xSemaphoreGive(s.mutex);
        return false;
      }
#if RICKYOS_PLUS_TX_POWER_COMPAT
      int8_t previousPower = 0;
      if (esp_wifi_get_max_tx_power(&previousPower) != ESP_OK) {
        LOG_ERR("WIFI", "Failed to read TX power for radio lease");
        xSemaphoreGive(s.mutex);
        return false;
      }
#endif
      if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        LOG_ERR("WIFI", "Failed to acquire radio-awake lease");
        xSemaphoreGive(s.mutex);
        return false;
      }
#if RICKYOS_PLUS_TX_POWER_COMPAT
      // This Plus reproducibly loses initial data frames at 20/13 dBm while
      // 5 dBm succeeds. Limit only active network owners, never raise a lower
      // pre-existing limit, and restore the first owner's original value.
      constexpr int8_t cap = 20;  // Quarter dBm: 5 dBm.
      const int8_t requested = previousPower < cap ? previousPower : cap;
      int8_t applied = previousPower;
      if (requested != previousPower &&
          (esp_wifi_set_max_tx_power(requested) != ESP_OK ||
           esp_wifi_get_max_tx_power(&applied) != ESP_OK || applied > requested)) {
        (void)esp_wifi_set_max_tx_power(previousPower);
        (void)esp_wifi_set_ps(previous);
        LOG_ERR("WIFI", "Failed to apply TX power lease; previous state restored where possible");
        xSemaphoreGive(s.mutex);
        return false;
      }
#endif
      if (s.owners == 0) {
        s.previous = previous;
#if RICKYOS_PLUS_TX_POWER_COMPAT
        s.previousPower = previousPower;
        s.appliedPower = applied;
#endif
      }
      ++s.owners;
      held_ = true;
      LOG_INF("WIFI", "Radio awake acquired: owners=%u", s.owners);
#if RICKYOS_PLUS_TX_POWER_COMPAT
      LOG_INF("WIFI", "TX power lease: previous=%d applied=%d owners=%u", previousPower, applied, s.owners);
#endif
    }
    xSemaphoreGive(s.mutex);
#if RICKYOS_PLUS_NET_ALLOC_TRACE
    plusNetAllocTraceSnapshot("lease-acquire");
#endif
    return true;
  }

  void release() {
    if (!held_) return;
    auto& s = state();
    if (!s.mutex || xSemaphoreTake(s.mutex, portMAX_DELAY) != pdTRUE) return;
    if (held_) {
      held_ = false;
      --s.owners;
      if (s.owners == 0) {
#if RICKYOS_PLUS_TX_POWER_COMPAT
        int8_t currentPower = 0;
        // A separate caller changing the setting takes ownership of its value.
        if (s.previousPower != s.appliedPower && esp_wifi_get_max_tx_power(&currentPower) == ESP_OK &&
            currentPower == s.appliedPower) {
          const esp_err_t result = esp_wifi_set_max_tx_power(s.previousPower);
          LOG_INF("WIFI", "Last TX power owner released: restored=%d result=%d", s.previousPower, result);
        }
#endif
        wifi_ps_type_t current;
        if (esp_wifi_get_ps(&current) == ESP_OK && current == WIFI_PS_NONE) {
          const esp_err_t result = esp_wifi_set_ps(s.previous);
          LOG_INF("WIFI", "Last radio owner released: restored=%u result=%d", s.previous, result);
        }
      } else {
        LOG_INF("WIFI", "Radio awake retained: owners=%u", s.owners);
      }
    }
    xSemaphoreGive(s.mutex);
#if RICKYOS_PLUS_NET_ALLOC_TRACE
    plusNetAllocTraceSnapshot("lease-release");
#endif
  }

  static unsigned activeCount() {
    auto& s = state();
    if (!s.mutex || xSemaphoreTake(s.mutex, portMAX_DELAY) != pdTRUE) return 0;
    const unsigned count = s.owners;
    xSemaphoreGive(s.mutex);
    return count;
  }
};
