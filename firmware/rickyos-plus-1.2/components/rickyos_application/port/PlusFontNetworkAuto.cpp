// A finite, explicitly enabled device diagnostic. No UI tapping or font install.
#if RICKYOS_PLUS_NET_AUTORUN
#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MappedInputManager.h>
#include <Memory.h>
#include <Network.h>
#include <NetworkStartup.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>
#if RICKYOS_PLUS_NET_HEAP_POLICY
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#endif
#include "PlusNetAllocTrace.h"
#include "activities/settings/FontDownloadActivity.h"
#if RICKYOS_PLUS_NET_UI_AUTORUN
#include "activities/ActivityManager.h"
#include "activities/settings/FontLibraryActivity.h"
#endif

extern MappedInputManager mappedInputManager;
#if RICKYOS_PLUS_NET_BODY_AUTORUN
extern bool plusNetworkFontBodyOnConnected();
extern int plusNetworkWeReadOnConnected(unsigned round);
#endif

class PlusFontNetworkAutoRunner {
#if RICKYOS_PLUS_NET_UI_AUTORUN
  static bool drainTransitions() {
    // A render waiter is notified before its RenderLock is destroyed. A single
    // manager.loop() may therefore defer a pop. Never overwrite that pending
    // pop with the next push and accidentally retain earlier font Activities.
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
      activityManager.loop();
      if (!activityManager.isSwitchPending()) return true;
      delay(10);
    }
    LOG_ERR("FONTNETAUTO", "UI transition did not drain; test stopped");
    return false;
  }
#endif
 public:
  static void run(GfxRenderer& renderer) {
    HalPowerManager::Lock performance;
#if RICKYOS_PLUS_NET_UI_AUTORUN
#if RICKYOS_PLUS_NET_AUTORUN_FROM_READER
    // This diagnostic is installed only after the user's completed download.
    // A saved reader boot is expected; exit through the real Activity lifecycle
    // before networking. Never navigate away from downloads, dialogs or login.
    if (activityManager.currentActivity && activityManager.currentActivity->isReaderActivity() &&
        !activityManager.isSwitchPending()) {
      activityManager.goHome();
      if (!drainTransitions()) return;
      activityManager.requestUpdateAndWait();
    }
#endif
    if (!activityManager.currentActivity || activityManager.currentActivity->mainTab() != MainTab::Recent ||
        activityManager.isSwitchPending()) {
      LOG_ERR("FONTNETAUTO", "UI comparison requires idle home; stopped without navigation");
      return;
    }
#endif
    wifi_config_t saved{};
    uint8_t protocols = 0;
    if (WiFi.status() != WL_CONNECTED || esp_wifi_get_config(WIFI_IF_STA, &saved) != ESP_OK ||
        esp_wifi_get_protocol(WIFI_IF_STA, &protocols) != ESP_OK) {
      LOG_ERR("FONTNETAUTO", "No connected station configuration; stopped");
      return;
    }
    const bool reconnect = WiFi.getAutoReconnect();
    WiFi.setAutoReconnect(false);
#if RICKYOS_PLUS_NET_HEAP_POLICY
    // Controlled test only. SDK heap_caps.c:118-125 otherwise prefers internal
    // memory for every ordinary allocation <=16 KiB. Existing allocations are
    // not relocated; DMA/explicit-internal allocations keep their capabilities.
    struct ScopedHeapPolicy {
      ScopedHeapPolicy() { heap_caps_malloc_extmem_enable(2048); }
      ~ScopedHeapPolicy() { heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL); }
    } heapPolicy;
    LOG_INF("FONTNETAUTO", "heap_policy internal_preference_limit=2048 restore_limit=%u",
            unsigned(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL));
#endif
#if RICKYOS_PLUS_NET_UI_AUTORUN
    // Preserve the actual retained parent screens and their allocation order.
    // The core-only test omitted them and had more free internal memory.
    halClock.cancelSync();
    WiFi.disconnect(true, false);
    activityManager.goToSettings();
    if (!drainTransitions()) {
      WiFi.setAutoReconnect(reconnect);
      return;
    }
    activityManager.requestUpdateAndWait();
#endif
    unsigned passed = 0, attempted = 0;
#if RICKYOS_PLUS_NET_BODY_AUTORUN
    unsigned bodyAttempted = 0, bodyPassed = 0;
    unsigned wereadAttempted = 0, wereadPassed = 0, wereadNeedsLogin = 0;
#endif
    const uint32_t started = millis();
    constexpr unsigned rounds = 12;
    constexpr uint32_t runBudgetMs = 20 * 60 * 1000;
    LOG_INF("FONTNETAUTO", "begin rounds=%u copy_available=%u object_bytes=%u", rounds,
            unsigned(RICKYOS_PLUS_NET_COPY_TX), unsigned(sizeof(FontDownloadActivity)));
    for (unsigned round = 1; round <= rounds && millis() - started < runBudgetMs; ++round) {
      halClock.cancelSync();
#if RICKYOS_PLUS_NET_UI_AUTORUN
      if (activityManager.isSwitchPending() || !activityManager.currentActivity ||
          activityManager.currentActivity->mainTab() != MainTab::Settings) {
        LOG_ERR("FONTNETAUTO", "Previous page not fully released; stopped");
        break;
      }
      auto library = makeUniqueNoThrow<FontLibraryActivity>(renderer, mappedInputManager);
      if (!library) break;
      auto* libraryParent = library.get();
      activityManager.pushActivity(std::move(library));
      if (!drainTransitions() || activityManager.currentActivity.get() != libraryParent) break;
      activityManager.requestUpdateAndWait();
      // Match the UI's allocation order: the font Activity exists before Wi-Fi starts.
      auto uiPage = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager);
      if (!uiPage) break;
#endif
      const uint32_t joinStart = millis();
      const unsigned phase = (round - 1) / 4;  // Four reference, four copy, four reference again.
#if RICKYOS_PLUS_NET_SECURITY_CHECK
      // Exercise Arduino's production begin() builder on every round rather
      // than replaying a driver-normalized configuration across deinit/init.
      const bool useCopy = false;
      const uint8_t roundProtocols = protocols;
#elif RICKYOS_PLUS_NET_AX_COMPARE
      // WifiSelectionActivity.cpp:601-605 pins b/g/n to work around an old HE
      // stall. Compare only this protocol mask, keeping the original TX API.
      const bool useCopy = false;
      const uint8_t roundProtocols = phase == 1 ? uint8_t(protocols | WIFI_PROTOCOL_11AX)
                                                : uint8_t(protocols & ~WIFI_PROTOCOL_11AX);
#else
      const bool useCopy = phase == 1;
      const uint8_t roundProtocols = protocols;
#endif
      // Mirror normal startup's stop/reinitialize sequence; credentials remain
      // on the device stack and are not logged or changed in the SD store.
      bool joined = NetworkStartup::setMode(renderer, WIFI_STA);
      if (joined) WiFi.disconnect(true, false);
#if RICKYOS_PLUS_NET_COPY_TX
      plusNetAllocTraceSetCopyTx(useCopy);
#endif
      joined = joined && NetworkStartup::setMode(renderer, WIFI_STA);
#if RICKYOS_PLUS_NET_SECURITY_CHECK
      joined = joined && esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK &&
               esp_wifi_set_protocol(WIFI_IF_STA, roundProtocols) == ESP_OK;
      if (joined) {
        char ssid[sizeof(saved.sta.ssid) + 1]{};
        char password[sizeof(saved.sta.password) + 1]{};
        std::memcpy(ssid, saved.sta.ssid, sizeof(saved.sta.ssid));
        std::memcpy(password, saved.sta.password, sizeof(saved.sta.password));
        WiFi.begin(ssid, password);
      }
#else
      wifi_config_t current = saved;
      joined = joined && esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK &&
               esp_wifi_set_protocol(WIFI_IF_STA, roundProtocols) == ESP_OK &&
               esp_wifi_set_config(WIFI_IF_STA, &current) == ESP_OK && WiFi.reconnect();
#endif
      while (joined && WiFi.status() != WL_CONNECTED && millis() - joinStart < 35000) delay(50);
      joined = joined && WiFi.status() == WL_CONNECTED && Network.setDefaultInterface(WiFi.STA);
      LOG_INF("FONTNETAUTO", "round=%u phase=%u copy_tx=%u joined=%u join_ms=%lu", round, phase,
              unsigned(useCopy), unsigned(joined),
              static_cast<unsigned long>(millis() - joinStart));
      if (!joined) {
        plusNetAllocTraceSnapshot("autorun-join-failed");
#if (RICKYOS_PLUS_NET_AX_COMPARE || RICKYOS_PLUS_NET_SECURITY_CHECK) && RICKYOS_PLUS_NET_UI_AUTORUN
        // Keep failed association samples and still reach the reverse b/g/n
        // group. Otherwise one HE join failure would erase the A/B/A check.
        uiPage.reset();
        if (activityManager.currentActivity.get() != libraryParent) break;
        activityManager.popActivity();
        if (!drainTransitions()) break;
        activityManager.requestUpdateAndWait();
        WiFi.disconnect(true, false);
        delay(750);
        continue;
#else
        break;
#endif
      }
      // Record the actual link security in every automatic regression run;
      // an ordinary WPA2 connection must not be reported as a WPA3 test.
      wifi_phy_mode_t negotiated{};
      wifi_ap_record_t ap{};
      const auto phyResult = esp_wifi_sta_get_negotiated_phymode(&negotiated);
      const auto apResult = esp_wifi_sta_get_ap_info(&ap);
      LOG_INF("FONTNETAUTO", "link round=%u phase=%u protocols=%u phy_result=%d phy=%u he20=%u ap_result=%d ap_ax=%u auth=%u",
              round, phase, unsigned(roundProtocols), phyResult, unsigned(negotiated),
              unsigned(phyResult == ESP_OK && negotiated == WIFI_PHY_MODE_HE20), apResult,
              unsigned(apResult == ESP_OK && ap.phy_11ax), unsigned(apResult == ESP_OK ? ap.authmode : 255));
      // As in the UI, NTP runs while the production font fetch/parser executes.
      const bool clockStarted = halClock.requestSync();
#if !RICKYOS_PLUS_NET_UI_AUTORUN
      NetworkStartup::prepare(renderer);
#endif
      bool ok = false, cleanup = false;
      size_t families = 0, files = 0;
      uint32_t requestMs = 0;
      {
        // Same object/JSON/vectors as production, destroyed each round so their
        // memory lifetime is represented. A single fallible allocation avoids
        // putting the Activity on the limited loop stack. No onEnter/onExit UI
        // events are run; this is a core-path comparison, not full UI acceptance.
#if RICKYOS_PLUS_NET_UI_AUTORUN
        auto page = std::move(uiPage);
#else
        auto page = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager);
#endif
        if (!page) {
          LOG_ERR("FONTNETAUTO", "round=%u object_allocation_failed", round);
          break;
        }
        {
          // The production parser uses this reserved temporary path. Claim it
          // exclusively first; never truncate/delete a pre-existing user file.
          auto reserved = Storage.open("/fonts_manifest.tmp", O_CREAT | O_EXCL | O_WRITE);
          if (!reserved) {
            LOG_ERR("FONTNETAUTO", "round=%u temporary_path_busy; stopped", round);
            break;
          }
        }
        const uint32_t requestStart = millis();
        ++attempted;
#if RICKYOS_PLUS_NET_UI_AUTORUN
        auto* observed = page.get();
        activityManager.pushActivity(std::move(page));
        const bool entered = drainTransitions();  // Real onEnter/loading/fetch/parse.
        const bool ours = entered && activityManager.currentActivity.get() == observed;
        if (ours) {
          ok = observed->state_ == FontDownloadActivity::GROUP_LIST || observed->state_ == FontDownloadActivity::FAMILY_LIST;
          families = observed->families_.size();
          files = observed->files_.size();
          activityManager.requestUpdateAndWait();  // Real gray UI/display/glyph-cache path.
#if RICKYOS_PLUS_NET_BODY_AUTORUN
          if (ok && (round == 1 || round == 9)) {
            ++bodyAttempted;
            const bool bodyOk = plusNetworkFontBodyOnConnected();
            bodyPassed += bodyOk;
            LOG_INF("FONTNETAUTO", "font_body round=%u ok=%u", round, unsigned(bodyOk));
          }
          if (ok && (round == 2 || round == 6 || round == 10)) {
            ++wereadAttempted;
            const int result = plusNetworkWeReadOnConnected(round);
            wereadPassed += result == 1;
            wereadNeedsLogin += result == 2;
          }
#endif
          activityManager.popActivity();
        } else {
          // Stop rather than guessing through a Wi-Fi selection or another popup.
          activityManager.goHome();
        }
        const bool exited = drainTransitions();  // Real onExit, destruction, parent resume.
        if (exited) activityManager.requestUpdateAndWait();
        const bool parent = activityManager.currentActivity.get() == libraryParent;
        if (!ours || !exited || !parent) {
          cleanup = !Storage.exists("/fonts_manifest.tmp") || Storage.remove("/fonts_manifest.tmp");
          LOG_ERR("FONTNETAUTO", "Unexpected activity during UI test; stopped");
          break;
        }
        activityManager.popActivity();  // Font library -> retained Settings, as in the user report.
        if (!drainTransitions()) break;
        activityManager.requestUpdateAndWait();
#else
        ok = page->fetchAndParseManifest();
        families = page->families_.size();
        files = page->files_.size();
#endif
        requestMs = millis() - requestStart;
        cleanup = !Storage.exists("/fonts_manifest.tmp") || Storage.remove("/fonts_manifest.tmp");
      }
      halClock.update();
      halClock.cancelSync();
      if (ok && cleanup) ++passed;
      plusNetAllocTraceSnapshot("autorun-round-end");
      LOG_INF("FONTNETAUTO", "round=%u phase=%u copy_tx=%u ok=%u cleanup=%u request_ms=%lu families=%u files=%u clock_started=%u",
              round, phase, unsigned(useCopy), unsigned(ok), unsigned(cleanup), static_cast<unsigned long>(requestMs),
              unsigned(families), unsigned(files), unsigned(clockStarted));
      if (!cleanup) break;
      WiFi.disconnect(false, false, 1500);
      delay(750);
    }
    halClock.cancelSync();
#if RICKYOS_PLUS_NET_UI_AUTORUN
    activityManager.goHome();
    if (drainTransitions()) activityManager.requestUpdateAndWait();
#endif
    WiFi.setAutoReconnect(reconnect);
    WiFi.disconnect(true, false);
#if RICKYOS_PLUS_NET_COPY_TX
    plusNetAllocTraceSetCopyTx(false);
#endif
    LOG_INF("FONTNETAUTO", "done attempted=%u passed=%u expected=%u elapsed_ms=%lu auto_reconnect_restored=%u",
            attempted, passed, rounds, static_cast<unsigned long>(millis() - started),
            unsigned(WiFi.getAutoReconnect() == reconnect));
#if RICKYOS_PLUS_NET_BODY_AUTORUN
    LOG_INF("FONTNETAUTO", "body_done attempted=%u passed=%u expected=2", bodyAttempted, bodyPassed);
    LOG_INF("FONTNETAUTO", "weread_done attempted=%u passed=%u needs_login=%u expected=3",
            wereadAttempted, wereadPassed, wereadNeedsLogin);
#endif
  }
};

void plusFontNetworkAutoRun(GfxRenderer& renderer) { PlusFontNetworkAutoRunner::run(renderer); }
#endif
