// Unattended network self-test for the Plus (built only with RICKYOS_PLUS_NETTEST=1).
// 30 s after boot it joins the last used network the way WifiSelectionActivity does,
// then fetches the hosts RickyOS uses (COS, WeRead, jsDelivr/Fastly, the site) for three
// rounds and logs, per request, whether data arrived, how much and how fast. Two 1 MB
// reads measure throughput from COS and from Fastly. Nothing is written to the card.
// HEADLESS skips application/UI startup. NATIVE uses the IDF STA bootstrap and
// resolver, BG restricts that native test to 11b/g. CONFIG_BUFFERS asks Arduino
// to honor sdkconfig instead of overriding RX buffers. These are diagnostic
// options only; none is enabled in a normal build.
#if RICKYOS_PLUS_NETTEST

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <Network.h>
#include <NetworkClient.h>
#include <esp_wifi.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <esp_event.h>
#include <esp_netif.h>
#include <lwip/netdb.h>


#include <string>

#include "HalWifiAwakeLease.h"
#include "HalPowerManager.h"
#include "HalWifiDiagnostics.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace {
volatile bool g_active = false;
#if RICKYOS_PLUS_NETTEST_HEADLESS
constexpr int kRounds = 2;
#else
constexpr int kRounds = 3;
#endif

struct Probe {
  const char* name;
  const char* url;
  size_t cap;  // stop reading after this many bytes
};

constexpr Probe kProbes[] = {
    {"cos-wallpaper-manifest", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/wallpapers/cos.json", 16 * 1024},
    {"cos-vendor-image", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/wallpapers/v1.3.0/00-rest.png", 16 * 1024},
    {"cos-manifest", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/fonts/cos.json", 16 * 1024},
    {"weread", "https://weread.qq.com/", 16 * 1024},
    {"fastly-manifest", "https://fastly.jsdelivr.net/gh/chinoryunqin/RickyOS-fonts@latest/mirror.json", 16 * 1024},
    {"site-ota", "https://chinoryunqin.github.io/RickyOS-site/ota.json", 16 * 1024},
    {"cos-1MB", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/fonts/v1.0.2/RickySans_12.cpfont",
     1024 * 1024},
    {"fastly-1MB", "https://fastly.jsdelivr.net/gh/chinoryunqin/RickyOS-fonts@v1.0.2/fonts/RickySans_12.cpfont",
     1024 * 1024},
};

constexpr char kCosHost[] = "rickyos-1309217881.cos.ap-guangzhou.myqcloud.com";

void rawTcp(const char* address, int round, uint16_t port = 443) {
  const uint32_t started = millis();
  int error = 0;
  bool connected = false;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    error = errno;
  } else {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &peer.sin_addr) != 1 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
      error = errno ? errno : EINVAL;
    } else if (connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
      connected = true;
    } else if (errno == EINPROGRESS) {
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(fd, &writable);
      timeval timeout{15, 0};
      const int ready = select(fd + 1, nullptr, &writable, nullptr, &timeout);
      if (ready > 0) {
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) error = errno;
        connected = error == 0;
      } else {
        error = ready == 0 ? ETIMEDOUT : errno;
      }
    } else {
      error = errno;
    }
    close(fd);
  }
  LOG_INF("NETTEST", "round=%d raw-tcp ip=%s port=%u ok=%d errno=%d ms=%lu", round, address, port, connected, error,
          static_cast<unsigned long>(millis() - started));
}

void arduinoTcp(const IPAddress& address, int round) {
  NetworkClient client;
  client.setConnectionTimeout(15000);
  const uint32_t started = millis();
  const bool connected = client.connect(address, 443);
  LOG_INF("NETTEST", "round=%d arduino-tcp ok=%d ms=%lu", round, connected,
          static_cast<unsigned long>(millis() - started));
  client.stop();
}

#if RICKYOS_PLUS_NETTEST_HEADLESS
void localTransfer(int round) {
  // Diagnostic host on this LAN; no DNS, TLS or external server is involved.
  constexpr char host[] = "192.168.5.3";
  rawTcp(host, round, 54321);
  NetworkClient client;
  client.setConnectionTimeout(15000);
  const uint32_t started = millis();
  const bool connected = client.connect(IPAddress(192, 168, 5, 3), 54321);
  size_t bytes = 0;
  uint8_t buffer[2048];  // bounded stack buffer, no retained download allocation
  if (connected) {
    client.print("GET /test HTTP/1.0\r\nHost: 192.168.5.3\r\n\r\n");
    while (millis() - started < 20000 && bytes < 1024 * 1024) {
      const int available = client.available();
      if (available > 0) {
        const int count = client.read(buffer, sizeof(buffer));
        if (count > 0) bytes += count;
      } else if (!client.connected()) break;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  LOG_INF("NETTEST", "round=%d LAN TCP=%d cap_reached=%d bytes=%u ms=%lu", round, connected,
          bytes >= 1024 * 1024, static_cast<unsigned>(bytes), static_cast<unsigned long>(millis() - started));
  client.stop();
}
#endif

void nativeTls(const char* address, int round) {
  // One transient IDF TLS context at a time; vendor settings place its allocations
  // in PSRAM. Fixed request/response buffers fit the existing 20 KiB task stack.
  const uint32_t started = millis();
  esp_tls_t* tls = esp_tls_init();
  if (!tls) {
    LOG_ERR("NETTEST", "native-tls allocation failed");
    return;
  }
  esp_tls_cfg_t config{};
  config.timeout_ms = 15000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  // Connect to exactly the IP tested above, retaining hostname verification/SNI.
  // Use the vendor's synchronous API, with an explicit bound for unattended tests.
  config.common_name = kCosHost;
  const int result = esp_tls_conn_new_sync(address, strlen(address), 443, &config, tls);
  const uint32_t handshakeMs = millis() - started;
  const uint32_t responseStarted = millis();
  char response[513]{};
  size_t received = 0;
  if (result == 1) {
    char request[256];
    const int length = snprintf(request, sizeof(request),
                                "GET /fonts/cos.json HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", kCosHost);
    size_t sent = 0;
    while (length > 0 && static_cast<size_t>(length) < sizeof(request) && sent < static_cast<size_t>(length) &&
           millis() - responseStarted < 12000) {
      const int n = esp_tls_conn_write(tls, request + sent, length - sent);
      if (n > 0) sent += n;
      else if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (received < sizeof(response) - 1 && millis() - responseStarted < 12000) {
      const int n = esp_tls_conn_read(tls, response + received, sizeof(response) - 1 - received);
      if (n > 0) {
        received += n;
        if (memchr(response, '\n', received)) break;
      } else if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  int tlsError = 0, flags = 0;
  esp_tls_conn_state_t state = ESP_TLS_INIT;
  (void)esp_tls_get_conn_state(tls, &state);
  esp_err_t error = ESP_OK;
  esp_tls_error_handle_t handle;
  if (esp_tls_get_error_handle(tls, &handle) == ESP_OK)
    error = esp_tls_get_and_clear_last_error(handle, &tlsError, &flags);
  if (char* end = strpbrk(response, "\r\n")) *end = '\0';
  LOG_INF("NETTEST", "round=%d native-tls ip=%s result=%d state=%d handshake_ms=%lu bytes=%u status=%s err=%x tls=%d flags=%x",
          round, address, result, static_cast<int>(state), static_cast<unsigned long>(handshakeMs), static_cast<unsigned>(received), response,
          static_cast<unsigned>(error), tlsError, flags);
  esp_tls_conn_destroy(tls);
}

void compareStacks(int round) {
  IPAddress address;
  int arduinoResult = 0;
  char nativeIp[16]{};
  auto arduinoDns = [&]() {
    const uint32_t began = millis();
    arduinoResult = Network.hostByName(kCosHost, address);
    LOG_INF("NETTEST", "round=%d arduino-DNS result=%d ms=%lu", round, arduinoResult,
            static_cast<unsigned long>(millis() - began));
  };
  auto nativeDns = [&]() {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const uint32_t began = millis();
    const int resolved = getaddrinfo(kCosHost, nullptr, &hints, &result);
    if (resolved == 0 && result) {
      const auto* peer = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
      inet_ntop(AF_INET, &peer->sin_addr, nativeIp, sizeof(nativeIp));
    }
    if (result) freeaddrinfo(result);
    LOG_INF("NETTEST", "round=%d native-DNS result=%d ip=%s ms=%lu", round, resolved, nativeIp,
            static_cast<unsigned long>(millis() - began));
  };
  if (round % 2) { arduinoDns(); nativeDns(); }
  else { nativeDns(); arduinoDns(); }
  if (arduinoResult != 1 || address.type() != IPv4) {
    if (!nativeIp[0] || !address.fromString(nativeIp)) {
      LOG_ERR("NETTEST", "round=%d both COS resolvers failed; no fixed-IP comparison", round);
      return;
    }
  }
  char ip[16];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
  LOG_INF("NETTEST", "round=%d stack comparison ip=%s internal_free=%u psram_free=%u", round, ip,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  // Alternate the first socket so COS is not always penalized by join/order effects.
  if (round % 2) { rawTcp(ip, round); arduinoTcp(address, round); }
  else { arduinoTcp(address, round); rawTcp(ip, round); }
  nativeTls(ip, round);
}

void run(const Probe& probe, int round) {
  size_t bytes = 0;
  const uint32_t started = millis();
  const bool ok = HttpDownloader::fetchUrl(probe.url, [&bytes, &probe](const uint8_t*, size_t len) {
    bytes += len;
    return bytes < probe.cap;  // false ends the transfer once enough has arrived
  });
  const uint32_t elapsed = millis() - started;
  LOG_INF("NETTEST", "round=%d %s ok=%d cap_reached=%d bytes=%u ms=%lu rate=%lu KB/s", round, probe.name, ok,
          bytes >= probe.cap, static_cast<unsigned>(bytes),
          static_cast<unsigned long>(elapsed), static_cast<unsigned long>(elapsed ? bytes / elapsed : 0));
}

#if RICKYOS_PLUS_NETTEST_NATIVE && RICKYOS_PLUS_NETTEST_HEADLESS
void nativeWifiTests(const std::string& network, const std::string& password) {
  // SDK allocates the transient netif/driver state, matching the vendor bootstrap.
  // Credentials are loaded through the existing store, never printed or persisted.
  if (network.size() > 32 || password.size() > 64) return;
  if (esp_netif_init() != ESP_OK) return;
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return;
  esp_netif_t* netif = esp_netif_create_default_wifi_sta();
  if (!netif) return;
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  init.nvs_enable = false;
  bool initialized = false;
  bool registered = false;
  std::atomic<bool> ready{false};
  esp_event_handler_instance_t handler{};
  auto tests = [&]() {
    if (esp_wifi_init(&init) != ESP_OK) return;
    initialized = true;
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        [](void* arg, esp_event_base_t, int32_t, void*) { static_cast<std::atomic<bool>*>(arg)->store(true); },
        &ready, &handler) != ESP_OK) return;
    registered = true;
    wifi_config_t config{};
    memcpy(config.sta.ssid, network.data(), network.size());
    memcpy(config.sta.password, password.data(), password.size());
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
#if RICKYOS_PLUS_NETTEST_BG
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G) != ESP_OK ||
#else
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N) != ESP_OK ||
#endif
        esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_start() != ESP_OK ||
        esp_wifi_connect() != ESP_OK) return;
    const uint32_t joined = millis();
    while (!ready.load() && millis() - joined < 30000) vTaskDelay(pdMS_TO_TICKS(100));
    LOG_INF("NETTEST", "native-IDF join=%d ms=%lu", ready.load(), static_cast<unsigned long>(millis()-joined));
    if (!ready.load()) return;
    esp_netif_set_default_netif(netif);
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_phy_mode_t phy;
    esp_wifi_sta_get_negotiated_phymode(&phy);
    LOG_INF("NETTEST", "native-IDF negotiated phy=%d", static_cast<int>(phy));
    for (int round = 1; round <= 2; ++round) {
      localTransfer(round);
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* result = nullptr;
      const uint32_t began = millis();
      const int resolved = getaddrinfo(kCosHost, nullptr, &hints, &result);
      char ip[16]{};
      if (resolved == 0 && result) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
      }
      if (result) freeaddrinfo(result);
      LOG_INF("NETTEST", "round=%d native-DNS result=%d ip=%s ms=%lu", round, resolved, ip,
              static_cast<unsigned long>(millis()-began));
      if (ip[0]) { rawTcp(ip, round); nativeTls(ip, round); }
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  };
  tests();
  if (initialized) { esp_wifi_stop(); esp_wifi_deinit(); }
  if (registered) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, handler);
  esp_netif_destroy_default_wifi(netif);
  LOG_INF("NETTEST", "done");
}
#endif

void runTests() {
  WIFI_STORE.loadFromFile();
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  const auto credential = WIFI_STORE.findCredential(ssid);
  if (ssid.empty() || !credential) {
    LOG_ERR("NETTEST", "no saved network to join");
    return;
  }
#if RICKYOS_PLUS_NETTEST_NATIVE && RICKYOS_PLUS_NETTEST_HEADLESS
  nativeWifiTests(ssid, credential->password);
  return;
#endif
#if RICKYOS_PLUS_NETTEST_CONFIG_BUFFERS
  WiFi.useStaticBuffers(true);  // honor WIFI_INIT_CONFIG_DEFAULT instead of Arduino 4/32 override
#endif
  plusWifiDiagnosticsBegin();
  WiFi.persistent(false);
  // Match normal WifiSelectionActivity's teardown/rebuild and scan policy.
  // Scan results are owned by Arduino and released before the join begins.
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("NETTEST", "STA init failed");
    return;
  }
  WiFi.disconnect(true, true);
  vTaskDelay(pdMS_TO_TICKS(100));
  bool joined = false;
  for (int attempt = 1; attempt <= 2 && !joined; ++attempt) {
    if (!WiFi.mode(WIFI_STA)) break;
#if RICKYOS_PLUS_NETTEST_AX
    const uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
#else
    const uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
#endif
    const esp_err_t protocolResult = esp_wifi_set_protocol(WIFI_IF_STA, protocols);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    const int count = WiFi.scanNetworks(false, false);
    bool visible = false;
    int signal = -127;
    for (int i = 0; i < count; ++i) {
      if (WiFi.SSID(i) == ssid.c_str()) {
        visible = true;
        signal = std::max(signal, static_cast<int>(WiFi.RSSI(i)));
      }
    }
    WiFi.scanDelete();
    LOG_INF("NETTEST", "join attempt=%d scan=%d saved_target_visible=%d rssi=%d protocol_result=%d",
            attempt, count, visible, signal, protocolResult);
    const uint32_t joinStarted = millis();
    const int beginResult = WiFi.begin(ssid.c_str(), credential->password.c_str());
    LOG_INF("NETTEST", "begin result=%d", beginResult);
    while (WiFi.status() != WL_CONNECTED && millis() - joinStarted < 45000)
      vTaskDelay(pdMS_TO_TICKS(200));
    joined = WiFi.status() == WL_CONNECTED;
    uint8_t activeProtocols = 0;
    esp_wifi_get_protocol(WIFI_IF_STA, &activeProtocols);
    wifi_phy_mode_t phy = WIFI_PHY_MODE_LR;
    if (joined) esp_wifi_sta_get_negotiated_phymode(&phy);
    LOG_INF("NETTEST", "join status=%d after %lu ms rssi=%d protocols=0x%x phy=%d", static_cast<int>(WiFi.status()),
            static_cast<unsigned long>(millis() - joinStarted), static_cast<int>(WiFi.RSSI()), activeProtocols,
            static_cast<int>(phy));
    if (!joined) {
      WiFi.disconnect(true, true);
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    HalWifiAwakeLease awake;  // no modem power save, as the app's own downloads run
    awake.acquire();
    for (int round = 1; round <= kRounds; ++round) {
      compareStacks(round);
      for (const auto& probe : kProbes) {
#if RICKYOS_PLUS_NETTEST_HEADLESS
        if (strcmp(probe.name, "weread") == 0 || strcmp(probe.name, "site-ota") == 0 ||
            probe.cap > 16 * 1024) continue;
#endif
        run(probe, round);
      }
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_INF("NETTEST", "done");
}

void task(void*) {
  vTaskDelay(pdMS_TO_TICKS(30000));
  g_active = true;
  const uint32_t before = getCpuFrequencyMhz();
  {
    // Auto-Standby suppression does not restore a CPU downclock from the first
    // 30 idle seconds. Restore performance before joining, and release normally
    // before deleting the FreeRTOS task (which does not unwind C++ scopes).
    HalPowerManager::Lock performance;
    LOG_INF("NETTEST", "CPU before=%lu MHz locked=%lu MHz", static_cast<unsigned long>(before),
            static_cast<unsigned long>(getCpuFrequencyMhz()));
    runTests();
  }
  g_active = false;
  vTaskDelete(nullptr);
}
}  // namespace

bool plusNetSelfTestActive() { return g_active; }

void plusNetSelfTestOnConnected() {
  if (WiFi.status() != WL_CONNECTED || g_active) return;
  g_active = true;
  {
    // Runs on the existing application network path, after its loading screen
    // has painted. No new task, scan array, credential copy or retained heap.
    HalPowerManager::Lock performance;
    HalWifiAwakeLease awake;
    if (awake.acquire()) {
      LOG_INF("NETTEST", "connected comparison begin CPU=%lu rssi=%d", getCpuFrequencyMhz(), WiFi.RSSI());
      for (int round = 1; round <= 2 && WiFi.status() == WL_CONNECTED; ++round) compareStacks(round);
      LOG_INF("NETTEST", "connected comparison complete; continuing normal manifest request");
    }
  }
  g_active = false;
}

void plusNetSelfTestStart() {
  // wolfSSL handshakes need a deep stack; the task exits when the test ends.
  if (xTaskCreate(task, "nettest", 20 * 1024, nullptr, 3, nullptr) != pdPASS)
    LOG_ERR("NETTEST", "could not allocate self-test task");
}

#endif
