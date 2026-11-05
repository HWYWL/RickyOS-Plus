// Real download/SD/clock paths, enabled only for unattended hardware acceptance.
#if RICKYOS_PLUS_NETWORK_ACCEPTANCE
#include <Arduino.h>
#include <Logging.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstdio>
#include <cstring>

#include "HalClock.h"
#include "HalPowerManager.h"
#include "HalStorage.h"
#include "HalWifiAwakeLease.h"
#include "network/HttpDownloader.h"
#include <Memory.h>
#include <WeReadClient.h>

#if RICKYOS_PLUS_TCP_TRACE
extern void plusTcpTraceBegin();
extern void plusTcpTraceEnd();
#endif

namespace WeReadClient {
// Existing test peer; this diagnostic only observes phase before a step. Stop
// before renewal/login can clear or replace the device's saved account.
struct OperationTestPeer {
  static bool authenticationPending(const Operation& op) {
    using Phase = Operation::Phase;
    return op.phase_ == Phase::Renew || op.phase_ == Phase::LoginUid ||
           op.phase_ == Phase::LoginPollWait || op.phase_ == Phase::LoginPoll;
  }
};
}

namespace {
constexpr char cosHost[] = "rickyos-1309217881.cos.ap-guangzhou.myqcloud.com";
struct Download {
  const char* name;
  const char* url;
  uint32_t bytes;
  uint32_t crc;
  bool index;
};
constexpr Download wallpaper{"wallpapers", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/wallpapers/cos.json",
                             3965, 252721562, true};
constexpr Download fonts{"fonts", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/fonts/cos.json",
                         9024, 2970146193u, true};
constexpr Download font{"font12", "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/fonts/v1.0.2/RickySans_12.cpfont",
                        1763452, 2837099957u, false};

uint32_t updateCrc(uint32_t crc, const uint8_t* bytes, size_t length) {
  while (length--) {
    crc ^= *bytes++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return crc;
}

bool restartNetwork(const wifi_config_t& original, uint8_t protocols, const char* name) {
  const uint32_t started = millis();
  wifi_config_t config = original;  // SDK setter is mutable; keep original intact.
  if (!WiFi.mode(WIFI_OFF) || !WiFi.mode(WIFI_STA) ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
      esp_wifi_set_protocol(WIFI_IF_STA, protocols) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || !WiFi.reconnect()) {
    LOG_ERR("ACCEPT", "name=%s restart_failed", name);
    return false;
  }
  while (WiFi.status() != WL_CONNECTED && millis() - started < 35000) delay(50);
  const bool joined = WiFi.status() == WL_CONNECTED;
  const bool route = joined && Network.setDefaultInterface(WiFi.STA);
  LOG_INF("ACCEPT", "name=%s joined=%d route=%d join_ms=%lu rssi=%d", name, joined, route,
          static_cast<unsigned long>(millis() - started), joined ? static_cast<int>(WiFi.RSSI()) : -127);
  return route;
}

bool reserveTemp(const char* name, char* path, size_t capacity) {
  for (unsigned id = 0; id < 16; ++id) {
    const int size = snprintf(path, capacity, "/.crosspoint/plus-net-accept-20261016-%s-%u.tmp", name, id);
    if (size <= 0 || static_cast<size_t>(size) >= capacity) return false;
    // Exclusive creation gives ownership; never truncate an existing user file.
    auto reserved = Storage.open(path, O_CREAT | O_EXCL | O_WRITE);
    if (reserved) return true;  // Scope destructor closes before downloader opens.
  }
  return false;
}

bool download(const Download& item) {
  char path[96];
  if (!reserveTemp(item.name, path, sizeof(path))) {
    LOG_ERR("ACCEPT", "name=%s temp_reservation_failed", item.name);
    return false;
  }
  struct Context {
    uint32_t started;
    size_t lastProgress = 0;
    bool cancel = false;
    bool onlyCos = true;
  } context{millis()};
#if RICKYOS_PLUS_TCP_TRACE
  plusTcpTraceBegin();
#endif
  const auto result = item.index ? HttpDownloader::downloadIndex(item.url, path)
      : HttpDownloader::downloadToFile(item.url, path,
          [&context](size_t bytes, size_t total) {
            if (bytes >= context.lastProgress && bytes - context.lastProgress >= 256 * 1024) {
              context.lastProgress = bytes;
              LOG_INF("ACCEPT", "font_progress bytes=%u total=%u ms=%lu", static_cast<unsigned>(bytes),
                      static_cast<unsigned>(total), static_cast<unsigned long>(millis() - context.started));
            }
          }, &context.cancel, "", "", {}, false,
          [&context]() { if (millis() - context.started >= 90000) context.cancel = true; },
          [](const char* host, void* state) {
            auto* value = static_cast<Context*>(state);
            value->onlyCos = value->onlyCos && host && std::strcmp(host, cosHost) == 0;
          }, &context);
  const uint32_t elapsed = millis() - context.started;
#if RICKYOS_PLUS_TCP_TRACE
  plusTcpTraceEnd();
#endif
  uint32_t bytes = 0, crc = 0xffffffffu;
  bool readOk = false;
  {
    HalFile file;
    if (Storage.openFileForRead("ACCEPT", path, file)) {
      uint8_t buffer[1024];  // Streaming integrity check, no full-font allocation.
      for (;;) {
        const int count = file.read(buffer, sizeof(buffer));
        if (count == 0) { readOk = true; break; }
        if (count < 0 || static_cast<size_t>(count) > sizeof(buffer)) break;
        bytes += count;
        crc = updateCrc(crc, buffer, count);
      }
    }
  }
  crc ^= 0xffffffffu;
  const bool match = readOk && bytes == item.bytes && crc == item.crc;
  const bool cleanup = !Storage.exists(path) || Storage.remove(path);
  int8_t power = 0;
  const esp_err_t powerRead = esp_wifi_get_max_tx_power(&power);
  LOG_INF("ACCEPT", "name=%s result=%d download_ms=%lu bytes=%lu crc32=%lu content_match=%d cleanup=%d only_cos=%d",
          item.name, static_cast<int>(result), static_cast<unsigned long>(elapsed),
          static_cast<unsigned long>(bytes), static_cast<unsigned long>(crc), match, cleanup,
          item.index ? -1 : static_cast<int>(context.onlyCos));
  LOG_INF("ACCEPT", "name=%s after_power_read=%d after_power=%d radio_owners=%u", item.name, powerRead, power,
          HalWifiAwakeLease::activeCount());
  return result == HttpDownloader::OK && match && cleanup && (item.index || context.onlyCos);
}

// WeRead shelf sync through the production client: a logged-in device syncs its shelf
// (covers off), otherwise the run stops at the login QR, which is not logged.
bool weread(unsigned round) {
  // Operation holds the client's request buffers; heap like the activity that owns it.
  auto operation = makeUniqueNoThrow<WeReadClient::Operation>();
  if (!operation) {
    LOG_ERR("ACCEPT", "name=weread round=%u oom", round);
    return false;
  }
  int8_t powerBefore = 0;
  esp_wifi_get_max_tx_power(&powerBefore);
  const uint32_t started = millis();
  using Event = WeReadClient::Operation::Event;
  Event event = Event::None;
  if (operation->begin(WeReadClient::Operation::Kind::Sync)) {
    while (millis() - started < 60000) {
      event = operation->step();
      if (event == Event::QrReady || event == Event::Complete || event == Event::Failed ||
          event == Event::Cancelled)
        break;
    }
  }
  const uint32_t elapsed = millis() - started;
  const auto error = operation->error();
  if (event == Event::QrReady) operation->cancel();
  operation.reset();
  int8_t powerAfter = 0;
  esp_wifi_get_max_tx_power(&powerAfter);
  const bool ok = event == Event::QrReady || event == Event::Complete;
  LOG_INF("ACCEPT", "name=weread round=%u event=%u error=%d ms=%lu ok=%d power_before=%d power_after=%d", round,
          static_cast<unsigned>(event), static_cast<int>(error), static_cast<unsigned long>(elapsed), ok,
          powerBefore, powerAfter);
  return ok;
}
}

// Narrow entry for the finite font-page comparison. Reuse the production
// download/SD readback/CRC/owned-temp cleanup above; no WeRead operation or
// other acceptance scenario is run by this entry.
bool plusNetworkFontBodyOnConnected() { return download(font); }

// 0 = failed, 1 = authenticated shelf sync completed, 2 = login is required.
// No progress upload, login polling, logout, account switch or book download.
int plusNetworkWeReadOnConnected(unsigned round) {
  WeReadStore::Session saved;
  if (!WeReadStore::loadSession(saved) || !saved.valid()) {
    LOG_INF("ACCEPT", "weread_probe round=%u result=2 reason=login_required", round);
    return 2;
  }
  auto operation = makeUniqueNoThrow<WeReadClient::Operation>();
  if (!operation) {
    LOG_ERR("ACCEPT", "weread_probe round=%u result=0 reason=operation_oom", round);
    return 0;
  }
  using Event = WeReadClient::Operation::Event;
  auto run = [&](WeReadClient::Operation::Kind kind, const WeReadStore::ShelfRecord* book,
                 Event expected, const char* stage) {
    struct Budget { uint32_t started; } budget{millis()};
    WeReadHttpClient::AbortScope abort(
        [](void* state) { return millis() - static_cast<Budget*>(state)->started >= 60000; }, &budget);
    Event event = Event::None;
    bool authPending = false;
    if (operation->begin(kind, book)) {
      while (millis() - budget.started < 60000) {
        if (WeReadClient::OperationTestPeer::authenticationPending(*operation)) {
          authPending = true;
          break;
        }
        event = operation->step();
        if (event == expected || event == Event::Failed || event == Event::Cancelled || event == Event::QrReady) break;
        delay(10);
      }
    }
    const int result = authPending || event == Event::QrReady ? 2 : (event == expected ? 1 : 0);
    LOG_INF("ACCEPT", "weread_probe round=%u stage=%s result=%d event=%u error=%u ms=%lu", round, stage,
            result, unsigned(event), unsigned(operation->error()),
            static_cast<unsigned long>(millis() - budget.started));
    if (operation->active()) operation->cancel();
    return result;
  };
  const int result = run(WeReadClient::Operation::Kind::Sync, nullptr, Event::Complete, "shelf");
  if (result != 1) return result;
  uint32_t count = 0;
  WeReadStore::ShelfRecord first;
  bool readable = false, haveBook = false;
  {
    HalFile file;
    readable = WeReadStore::openShelf(file, count);
    haveBook = readable && count && WeReadStore::readShelfRecord(file, 0, first);
  }
  LOG_INF("ACCEPT", "weread_probe round=%u stage=cache readable=%u count=%lu", round, unsigned(readable),
          static_cast<unsigned long>(count));
  if (!readable || (count && !haveBook)) return 0;
  if (haveBook) {
    const int detail = run(WeReadClient::Operation::Kind::Detail, &first, Event::DetailReady, "detail");
    if (detail != 1) return detail;
  }
  return 1;
}

void plusNetworkAcceptanceOnConnected() {
  HalPowerManager::Lock performance;
  halClock.cancelSync();  // Each test starts without another radio owner/warmup.
  wifi_config_t original{};
  uint8_t protocols = 0;
  if (esp_wifi_get_config(WIFI_IF_STA, &original) != ESP_OK ||
      esp_wifi_get_protocol(WIFI_IF_STA, &protocols) != ESP_OK) {
    LOG_ERR("ACCEPT", "configuration_unavailable");
    return;
  }
  const bool reconnect = WiFi.getAutoReconnect();
  WiFi.setAutoReconnect(false);
  LOG_INF("ACCEPT", "begin cold_downloads=4 native_clock=1");
  unsigned passed = 0;
  for (const auto* item : {&wallpaper, &fonts, &font}) {
    if (restartNetwork(original, protocols, item->name) && download(*item)) ++passed;
  }
  if (restartNetwork(original, protocols, "clock")) {
    const uint32_t started = millis();
    const bool began = halClock.requestSync();
    while (began && halClock.syncState() == ClockSyncState::Syncing && millis() - started < 55000) {
      halClock.update();
      delay(50);
    }
    const bool success = began && halClock.syncState() == ClockSyncState::Succeeded;
    const auto epoch = halClock.nowUtc();
    halClock.cancelSync();
    LOG_INF("ACCEPT", "clock began=%d success=%d valid_time=%d epoch=%lld ms=%lu radio_owners=%u", began, success,
            halClock.hasValidTime(), static_cast<long long>(epoch),
            static_cast<unsigned long>(millis() - started), HalWifiAwakeLease::activeCount());
  }
  if (restartNetwork(original, protocols, "wallpapers-repeat") && download(wallpaper)) ++passed;
  unsigned wereadPassed = 0;
  for (unsigned round = 1; round <= 3; ++round) {
    if (restartNetwork(original, protocols, "weread") && weread(round)) ++wereadPassed;
  }
  LOG_INF("ACCEPT", "weread_passed=%u expected=3", wereadPassed);
  WiFi.setAutoReconnect(reconnect);
  LOG_INF("ACCEPT", "done downloads_passed=%u expected=4 auto_reconnect_restored=%d", passed,
          WiFi.getAutoReconnect() == reconnect);
}
#endif
