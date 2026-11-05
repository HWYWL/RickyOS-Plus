#pragma once
#include <esp_ota_ops.h>
using wifi_ps_type_t = int;
inline constexpr int WIFI_PS_NONE = 0;
inline constexpr int WIFI_PS_MIN_MODEM = 1;
inline int esp_wifi_get_ps(int* value) {
  *value = WIFI_PS_MIN_MODEM;
  return 0;
}
inline int esp_wifi_set_ps(int) { return 0; }
