// Unattended diagnostic bootstrap, omitted from ordinary firmware.
#if RICKYOS_PLUS_RADIO_AUTOSTART
#include <Arduino.h>
#include <Logging.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>

#include "HalPowerManager.h"
#include "HalWifiDiagnostics.h"
#include "PlusNetAllocTrace.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"

#if RICKYOS_PLUS_NET_PRIVATE_PROFILE
#include "PlusPrivateNetworkProfile.h"
#endif

#if RICKYOS_PLUS_OTA_SELFTEST
extern void plusOtaSelfTestOnConnected();
#elif RICKYOS_PLUS_NET_AUTORUN
extern void plusFontNetworkAutoRun(GfxRenderer& renderer);
#else
extern void plusRadioCompareOnConnected();
#endif

void plusRadioAutoStart(GfxRenderer& renderer) {
  bool connected = false;
  {
    // Release this lock before the comparison obtains its own performance lock.
    HalPowerManager::Lock performance;
    plusWifiDiagnosticsBegin();
#if RICKYOS_PLUS_NET_ALLOC_TRACE
    plusNetAllocTraceSnapshot("autorun-bootstrap");
#endif
#if RICKYOS_PLUS_NET_PRIVATE_PROFILE
    connected = plusPrivateNetworkBootstrap(renderer);
#else
    WIFI_STORE.loadFromFile();
#if RICKYOS_PLUS_NET_AUTORUN
    const auto lastNetwork = WIFI_STORE.getLastConnectedSsid();
    if (lastNetwork.empty()) {
      LOG_ERR("FONTNETAUTO", "No previous network; automatic comparison not started");
      return;
    }
#endif
    const size_t saved = std::min(WIFI_STORE.getCredentialCount(), size_t(8));
    LOG_INF("RADIOAUTO", "begin saved_profiles=%u", static_cast<unsigned>(saved));
    struct Candidate { size_t profile; int32_t rssi; };
    // At most eight store profiles. SDK scan results and optional store snapshots
    // are temporary API-owned allocations; delete scan results before joining.
    // Password snapshots are requested only for a selected candidate and stay
    // on-device. No extra vector, credential persistence or new task is needed.
    Candidate candidates[8]{};
    for (unsigned round = 1; saved && round <= 2 && !connected; ++round) {
      if (!NetworkStartup::setMode(renderer, WIFI_STA)) break;
      WiFi.disconnect(true, false);
      if (!NetworkStartup::setMode(renderer, WIFI_STA)) break;
      const esp_err_t protocol = esp_wifi_set_protocol(
          WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
      if (protocol != ESP_OK) break;
      WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
      WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
      const int scan = WiFi.scanNetworks(false, false);
      size_t count = 0;
      for (size_t i = 0; i < saved; ++i) {
        const auto name = WIFI_STORE.getSsidAt(i);
        if (!name) continue;
#if RICKYOS_PLUS_NET_AUTORUN
        if (*name != lastNetwork) continue;  // Do not silently compare a different saved network.
#endif
        int32_t best = -127;
        for (int ap = 0; ap < scan; ++ap)
          if (*name == WiFi.SSID(ap).c_str()) best = std::max(best, WiFi.RSSI(ap));
        if (best > -127) candidates[count++] = {i, best};
      }
      WiFi.scanDelete();
      std::sort(candidates, candidates + count,
                [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });
      LOG_INF("RADIOAUTO", "round=%u scan=%d visible_saved=%u", round, scan, static_cast<unsigned>(count));
      for (size_t i = 0; i < count && !connected; ++i) {
        const auto credential = WIFI_STORE.getCredentialAt(candidates[i].profile);
        if (!credential || credential->ssid.empty() || credential->ssid.size() > 32 ||
            credential->password.size() > 64) continue;
        WiFi.disconnect(false, false, 1500);
        const uint32_t started = millis();
        WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
        while (WiFi.status() != WL_CONNECTED && millis() - started < 35000) delay(100);
        connected = WiFi.status() == WL_CONNECTED;
        LOG_INF("RADIOAUTO", "round=%u profile=%u rssi=%d connected=%d join_ms=%lu", round,
                static_cast<unsigned>(candidates[i].profile), candidates[i].rssi, connected,
                static_cast<unsigned long>(millis() - started));
        if (!connected) WiFi.disconnect(false, false, 1500);
      }
      if (!connected && round < 2) delay(5000);
    }
#endif
  }
  if (connected) {
    const bool route = Network.setDefaultInterface(WiFi.STA);
    LOG_INF("RADIOAUTO", "default_sta_route=%d", route);
#if RICKYOS_PLUS_OTA_SELFTEST
    if (route) plusOtaSelfTestOnConnected();
#elif RICKYOS_PLUS_NET_AUTORUN
    if (route) plusFontNetworkAutoRun(renderer);
#else
    if (route) plusRadioCompareOnConnected();
#endif
  }
  LOG_INF("RADIOAUTO", "done joined=%d", connected);
}
#endif
