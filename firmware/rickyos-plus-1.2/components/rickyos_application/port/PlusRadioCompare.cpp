// Diagnostic only: compare protocols after the normal UI has associated.
// No saved credentials, application payload or SD files are read/written here.
#if RICKYOS_PLUS_RADIO_COMPARE
#include <Arduino.h>
#include <Logging.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <cerrno>
#if RICKYOS_PLUS_RADIO_RATE
#include <esp_private/wifi.h>
extern void plusRadioOverrideTxAggregation(bool disabled);
#endif

#include "HalPowerManager.h"
#include "HalWifiAwakeLease.h"

extern void plusTcpTraceBegin();
extern void plusTcpTraceEnd();

namespace {
constexpr uint8_t bg = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
constexpr uint8_t bgn = bg | WIFI_PROTOCOL_11N;
constexpr uint8_t bgnax = bgn | WIFI_PROTOCOL_11AX;
constexpr uint32_t joinBudget = 35000;
constexpr uint32_t tcpBudget = 15000;

#if RICKYOS_PLUS_RADIO_RATE
bool fixedRateActive = false;
bool restartRadio(const wifi_config_t& config, uint8_t mask, bool noAggregation, bool fixed,
                  const char* phase) {
  // Do not assume deinit clears the private fixed-rate state. Disable it while
  // the previous non-AMPDU driver is still running, before restoring AMPDU.
  if (fixedRateActive) {
    const esp_err_t clear = esp_wifi_internal_set_fix_rate(WIFI_IF_STA, false, WIFI_PHY_RATE_6M);
    LOG_INF("RADIO", "phase=%s fixed_rate_clear=%d", phase, clear);
    if (clear != ESP_OK) return false;
    fixedRateActive = false;
  }
  plusRadioOverrideTxAggregation(noAggregation);
  const uint32_t started = millis();
  // SDK's setter takes a mutable pointer; preserve the original snapshot for
  // later phases and restoration without casting away const.
  wifi_config_t phaseConfig = config;
  if (!WiFi.mode(WIFI_OFF) || !WiFi.mode(WIFI_STA) ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
      esp_wifi_set_protocol(WIFI_IF_STA, mask) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_STA, &phaseConfig) != ESP_OK) {
    LOG_ERR("RADIO", "phase=%s full_restart_failed", phase);
    return false;
  }
  const bool began = WiFi.reconnect();
  while (began && WiFi.status() != WL_CONNECTED && millis() - started < joinBudget) delay(50);
  const bool connected = WiFi.status() == WL_CONNECTED;
  const bool route = connected && Network.setDefaultInterface(WiFi.STA);
  // The private SDK API affects management/data frames and rejects TX AMPDU.
  // Its disabled/default state is obtained by a full driver restart above.
  const esp_err_t rate = fixed && connected
      ? esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true, WIFI_PHY_RATE_6M) : ESP_OK;
  if (fixed && connected && rate == ESP_OK) fixedRateActive = true;
  LOG_INF("RADIO", "phase=%s connected=%d route=%d no_tx_ampdu=%d fixed6=%d rate_result=%d join_ms=%lu rssi=%d",
          phase, connected, route, noAggregation, fixed, rate,
          static_cast<unsigned long>(millis() - started), connected ? static_cast<int>(WiFi.RSSI()) : -127);
  return connected && route && rate == ESP_OK;
}
#endif

bool join(uint8_t mask, const char* phase) {
  const uint32_t started = millis();
  // Preserve the current STA configuration; neither stop STA nor erase the AP.
  if (!WiFi.disconnect(false, false, 1500)) {
    LOG_ERR("RADIO", "phase=%s disconnect_failed", phase);
    return false;
  }
  const esp_err_t set = esp_wifi_set_protocol(WIFI_IF_STA, mask);
  if (set != ESP_OK) {
    LOG_ERR("RADIO", "phase=%s protocol_set=%d mask=0x%x", phase, set, mask);
    return false;
  }
  const bool began = WiFi.reconnect();
  while (began && WiFi.status() != WL_CONNECTED && millis() - started < joinBudget)
    delay(50);
  const bool connected = WiFi.status() == WL_CONNECTED;
  wifi_phy_mode_t phy = WIFI_PHY_MODE_LR;
  const esp_err_t phyResult = connected ? esp_wifi_sta_get_negotiated_phymode(&phy) : ESP_FAIL;
  LOG_INF("RADIO", "phase=%s mask=0x%x began=%d connected=%d join_ms=%lu phy_result=%d phy=%d rssi=%d",
          phase, mask, began, connected, static_cast<unsigned long>(millis() - started), phyResult,
          static_cast<int>(phy), connected ? static_cast<int>(WiFi.RSSI()) : -127);
  return connected;
}

void probe(const IPAddress& address, const char* phase, unsigned attempt) {
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(443);
  // Bytewise write is independent of IPAddress's integer conversion endianness.
  auto* octets = reinterpret_cast<uint8_t*>(&peer.sin_addr.s_addr);
  for (unsigned i = 0; i < 4; ++i) octets[i] = address[i];
  const uint32_t started = millis();
  bool connected = false;
  int error = 0;
  plusTcpTraceBegin();
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) error = errno;
  else {
    if (fd >= FD_SETSIZE) error = EMFILE;
    else if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) error = errno;
    else if (connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) connected = true;
    else if (errno != EINPROGRESS) error = errno;
    else {
      error = ETIMEDOUT;
      while (WiFi.status() == WL_CONNECTED) {
        const uint32_t elapsed = millis() - started;
        if (elapsed >= tcpBudget) break;
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        const uint32_t remaining = tcpBudget - elapsed;
        timeval timeout{};
        timeout.tv_usec = (remaining < 100 ? remaining : 100) * 1000;
        const int ready = select(fd + 1, nullptr, &writable, nullptr, &timeout);
        if (ready > 0) {
          socklen_t size = sizeof(error);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) error = errno;
          connected = error == 0;
          break;
        }
        if (ready < 0 && errno != EINTR) { error = errno; break; }
        delay(1);
      }
    }
    close(fd);
  }
  const uint32_t elapsed = millis() - started;
  // Include close-related completions in this probe instead of the next window.
  delay(500);
  plusTcpTraceEnd();
  LOG_INF("RADIO", "phase=%s attempt=%u tcp_ok=%d errno=%d tcp_ms=%lu", phase, attempt, connected, error,
          static_cast<unsigned long>(elapsed));
}
}

void plusRadioCompareOnConnected() {
#if RICKYOS_PLUS_NETWORK_ACCEPTANCE
  extern void plusNetworkAcceptanceOnConnected();
  plusNetworkAcceptanceOnConnected();
  return;
#else
  // Runs synchronously on the existing network call stack, not a new task.
  // All diagnostic state is stack/static; socket resources are closed per probe.
  uint8_t original = 0;
  if (WiFi.status() != WL_CONNECTED || esp_wifi_get_protocol(WIFI_IF_STA, &original) != ESP_OK) {
    LOG_ERR("RADIO", "not_connected_or_protocol_unavailable");
    return;
  }
  HalPowerManager::Lock performance;
  HalWifiAwakeLease awake;
  if (!awake.acquire()) return;
#if RICKYOS_PLUS_RADIO_TIMING
  // Previously observed COS peer; this raw TCP diagnostic deliberately avoids
  // DNS so a resolver failure cannot prevent the association timing comparison.
  // It is not a production endpoint override or a certificate validation path.
  const IPAddress address(36, 248, 13, 174);
  LOG_INF("RADIO", "fixed_destination bypass_dns=1 ip=36.248.13.174 original_mask=0x%x", original);
#else
  IPAddress address;
  const uint32_t dnsStarted = millis();
  const int dns = Network.hostByName("rickyos-1309217881.cos.ap-guangzhou.myqcloud.com", address);
  LOG_INF("RADIO", "fixed_destination DNS=%d ms=%lu ip=%u.%u.%u.%u original_mask=0x%x", dns,
          static_cast<unsigned long>(millis() - dnsStarted), address[0], address[1], address[2], address[3], original);
  if (dns != 1) return;
#endif
  const bool autoReconnect = WiFi.getAutoReconnect();
  WiFi.setAutoReconnect(false);
#if RICKYOS_PLUS_RADIO_RATE
  wifi_config_t savedConfig{};  // Bounded SDK struct; credentials never leave the device.
  if (esp_wifi_get_config(WIFI_IF_STA, &savedConfig) != ESP_OK) {
    WiFi.setAutoReconnect(autoReconnect);
    LOG_ERR("RADIO", "station_config_unavailable");
    return;
  }
  struct Phase { const char* name; bool noAggregation; bool fixed; int power = -1; };
#if RICKYOS_PLUS_RADIO_POWER
  int8_t originalPower = 0;
  if (esp_wifi_get_max_tx_power(&originalPower) != ESP_OK) {
    WiFi.setAutoReconnect(autoReconnect);
    LOG_ERR("RADIO", "original_power_unavailable");
    return;
  }
  LOG_INF("RADIO", "original_max_tx_power_quarter_dbm=%d", originalPower);
  const Phase phases[] = {{"power-default", false, false, -1},
                          {"power-same-explicit", false, false, originalPower},
                          {"power-cap13", false, false, originalPower < 52 ? originalPower : 52},
                          {"power-cap5", false, false, originalPower < 20 ? originalPower : 20},
                          {"power-original-repeat", false, false, originalPower}};
#else
  const Phase phases[] = {{"normal-before", false, false}, {"no-tx-ampdu", true, false},
                          {"fixed6-no-tx-ampdu", true, true}, {"normal-repeat", false, false}};
#endif
  for (const auto& phase : phases) {
    if (!restartRadio(savedConfig, original, phase.noAggregation, phase.fixed, phase.name)) continue;
#if RICKYOS_PLUS_RADIO_POWER
    const esp_err_t powerSet = phase.power >= 0 ? esp_wifi_set_max_tx_power(phase.power) : ESP_OK;
    int8_t actualPower = 0;
    const esp_err_t powerRead = esp_wifi_get_max_tx_power(&actualPower);
    LOG_INF("RADIO", "phase=%s power_request=%d power_set_result=%d power_read_result=%d power_actual_quarter_dbm=%d",
            phase.name, phase.power, powerSet, powerRead, actualPower);
    if (powerSet != ESP_OK || powerRead != ESP_OK) continue;
#endif
    const esp_err_t psSet = esp_wifi_set_ps(WIFI_PS_NONE);
    LOG_INF("RADIO", "phase=%s ps_none_result=%d probes_begin", phase.name, psSet);
    if (psSet != ESP_OK) continue;
    for (unsigned attempt = 1; attempt <= 2; ++attempt) probe(address, phase.name, attempt);
  }
  (void)restartRadio(savedConfig, original, false, false, "restore");
#if RICKYOS_PLUS_RADIO_POWER
  const esp_err_t powerRestore = esp_wifi_set_max_tx_power(originalPower);
  LOG_INF("RADIO", "power_restore=%d requested=%d", powerRestore, originalPower);
#endif
#else
  struct Phase { const char* name; uint8_t mask; bool reconnect; uint32_t waitMs; };
#if RICKYOS_PLUS_RADIO_TIMING
  // Identical protocol, fixed destination and two probes after each fresh join.
  // End with the immediate phase again to check for simple order/warmup effects.
  const Phase phases[] = {{"immediate", original, true, 0}, {"idle5", original, true, 5000},
                          {"idle20", original, true, 20000}, {"immediate-repeat", original, true, 0}};
#else
  const Phase phases[] = {{"original", original, false, 0}, {"bg", bg, true, 0},
                          {"bgnax", bgnax, true, 0}, {"bgn-repeat", bgn, true, 0}};
#endif
  for (const auto& phase : phases) {
    if (phase.reconnect && !join(phase.mask, phase.name)) continue;
    const esp_err_t psSet = esp_wifi_set_ps(WIFI_PS_NONE);
    LOG_INF("RADIO", "phase=%s ps_none_result=%d wait_ms=%lu", phase.name, psSet,
            static_cast<unsigned long>(phase.waitMs));
    if (psSet != ESP_OK) continue;
    if (phase.waitMs) delay(phase.waitMs);
    LOG_INF("RADIO", "phase=%s probes_begin", phase.name);
    for (unsigned attempt = 1; attempt <= 2; ++attempt) probe(address, phase.name, attempt);
  }
  uint8_t current = 0;
  if (esp_wifi_get_protocol(WIFI_IF_STA, &current) != ESP_OK || current != original ||
      WiFi.status() != WL_CONNECTED) (void)join(original, "restore");
#endif
  WiFi.setAutoReconnect(autoReconnect);
  uint8_t finalMask = 0;
  const esp_err_t restoredRead = esp_wifi_get_protocol(WIFI_IF_STA, &finalMask);
  LOG_INF("RADIO", "done connected=%d mask=0x%x protocol_restored=%d auto_reconnect_restored=%d",
          WiFi.status() == WL_CONNECTED, finalMask, restoredRead == ESP_OK && finalMask == original,
          WiFi.getAutoReconnect() == autoReconnect);
#endif
}
#endif
