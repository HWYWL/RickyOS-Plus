#include "HalWifiDiagnostics.h"
#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

void plusWifiDiagnosticsBegin() {
  static bool registered = false;
  if (registered) return;
  // One application-lifetime Arduino event registration; no per-request
  // allocation. Callback logs numeric state only, never SSIDs/credentials.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      LOG_INF("WIFIDIAG", "STA disconnected reason=%u", info.wifi_sta_disconnected.reason);
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED || event == ARDUINO_EVENT_WIFI_STA_GOT_IP ||
               event == ARDUINO_EVENT_WIFI_STA_LOST_IP || event == ARDUINO_EVENT_WIFI_STA_STOP) {
      LOG_INF("WIFIDIAG", "STA event=%u", static_cast<unsigned>(event));
    }
  });
  registered = true;
}

void plusWifiDiagnosticsSample() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 10000) return;
  last = now;
  wifi_ap_record_t ap{};
  wifi_ps_type_t ps = WIFI_PS_NONE;
  const auto apResult = esp_wifi_sta_get_ap_info(&ap);
  const auto psResult = esp_wifi_get_ps(&ps);
  LOG_INF("WIFIDIAG", "STA status=%d apResult=%d RSSI=%d channel=%u HT=%u psResult=%d ps=%u heap=%u",
          WiFi.status(), apResult, ap.rssi, ap.primary, ap.phy_11n, psResult, ps,
          static_cast<unsigned>(ESP.getFreeHeap()));
}
