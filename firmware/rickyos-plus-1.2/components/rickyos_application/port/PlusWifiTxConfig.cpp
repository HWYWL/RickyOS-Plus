#if RICKYOS_PLUS_RADIO_RATE
#include <esp_wifi.h>
#include <esp_log.h>

namespace { bool disableAggregation = false; }
void plusRadioOverrideTxAggregation(bool disabled) { disableAggregation = disabled; }

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t*);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  if (!config) return __real_esp_wifi_init(config);
  // Public SDK configuration copied on the existing call stack; no new heap.
  // Preserve every field except the explicitly tested TX aggregation flag.
  wifi_init_config_t controlled = *config;
  if (disableAggregation) controlled.ampdu_tx_enable = 0;
  ESP_LOGI("RADIOINIT", "tx_ampdu input=%d applied=%d config_bytes=%u", config->ampdu_tx_enable,
           controlled.ampdu_tx_enable, static_cast<unsigned>(sizeof(controlled)));
  return __real_esp_wifi_init(&controlled);
}
#endif
