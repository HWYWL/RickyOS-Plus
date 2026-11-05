// Plus-only adaptation at the Arduino/IDF boundary. All STA callers, including
// saved-network reconnects and resource downloads, receive the same SAE policy.
#include "sdkconfig.h"
#include "esp_wifi.h"

#if !CONFIG_ESP_WIFI_ENABLE_WPA3_SAE || !CONFIG_ESP_WIFI_ENABLE_SAE_H2E
#error "Plus requires WPA3-Personal with both SAE PWE methods; rebuild the configured sdkconfig"
#endif

extern "C" esp_err_t __real_esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* config) {
  if (interface != WIFI_IF_STA || config == nullptr || config->sta.password[0] == '\0') {
    return __real_esp_wifi_set_config(interface, config);
  }

  // The driver copies this synchronous input. A stack copy preserves caller
  // ownership and avoids a persistent buffer holding credentials or a new heap
  // allocation. Preserve explicit SAE choices and the caller's PMF requirement.
  wifi_config_t station = *config;
  station.sta.pmf_cfg.capable = true;
  if (station.sta.sae_pwe_h2e == WPA3_SAE_PWE_UNSPECIFIED) {
    station.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  }
  // PMF required=false permits older WPA2 APs; IDF still enforces PMF for WPA3.
  // Do not lower the auth threshold or automatically downgrade failed WPA3 joins.
  return __real_esp_wifi_set_config(interface, &station);
}
