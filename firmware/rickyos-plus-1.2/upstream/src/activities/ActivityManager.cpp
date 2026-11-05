#include "ActivityManager.h"

#if RICKYOS_PLUS_TAB_BENCH
#include <esp_timer.h>
#endif
#if RICKYOS_PLUS_SCREEN_TOUR
#include "CrossPointState.h"
#include "activities/reader/ReadingMode.h"
#if RICKYOS_PLUS_SCREEN_TOUR == 2
#include <esp_system.h>
#ifndef RICKYOS_PLUS_SCREEN_TOUR_FROM
#define RICKYOS_PLUS_SCREEN_TOUR_FROM 0
#endif
#include <string>
std::string plusTour2Book(const char* titlePart);
bool plusTour2Backup(const std::string& republic, const std::string& dance);
bool plusTour2Restore();
bool plusTour2Finished();
int plusTour2FindChapter(const std::string& path, const char* needle);
bool plusTour2SetProgress(const std::string& path, int spine);
bool plusTour2InstallSticker();
#endif
#include "RecentBooksStore.h"
#ifndef RICKYOS_PLUS_SCREEN_TOUR_FROM
#define RICKYOS_PLUS_SCREEN_TOUR_FROM 0
#endif
#ifndef RICKYOS_PLUS_SCREEN_TOUR_TO
#define RICKYOS_PLUS_SCREEN_TOUR_TO 7
#endif
#endif

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <Memory.h>
#include <VectorFontSupport.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "apps/AppsMenuActivity.h"
#include "apps/calculator/CalculatorActivity.h"
#if FREEINK_DEVICE_METALIO_PLUS
#include "apps/recorder/RecorderActivity.h"
#endif
#include "apps/gomoku/GomokuModeActivity.h"
#include "apps/reading-stats/RickyReadingStatsActivity.h"
#include "apps/standby/RickyStandbySettingsActivity.h"
#include "apps/standby/StandbyGalleryActivity.h"
#include "apps/standby/StandbyActivity.h"
#include "apps/weread/WeReadActivity.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "components/RickyDesignTokens.h"
#include "components/SubpageLayout.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/InxRecentActivity.h"
#include "home/RecentBooksActivity.h"
#include "home/RickyStorageActivity.h"
#include "library/LibraryListActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/UsbDriveActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"
#include "util/ImageViewerActivity.h"
#include "util/UserGuide.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

#if RICKYOS_PERF_SD_LOG
#include <HalStorage.h>
#include <cstdio>
#include <cstring>
namespace {
// Device timing for remote measurement: the render task queues one line per gray
// frame and the main loop appends them to the SD card (SdFat is not thread-safe).
constexpr const char* kPerfLogPath = "/rickyos-perf.log";
char perfPending[2048];
size_t perfPendingLen = 0;
unsigned long perfLastFlush = 0;
portMUX_TYPE perfSpinlock = portMUX_INITIALIZER_UNLOCKED;

void queuePerfLine(const char* line) {
  const size_t len = strnlen(line, 255);
  taskENTER_CRITICAL(&perfSpinlock);
  if (perfPendingLen + len <= sizeof(perfPending)) {
    memcpy(perfPending + perfPendingLen, line, len);
    perfPendingLen += len;
  }
  taskEXIT_CRITICAL(&perfSpinlock);
}

void flushPerfLog() {
  if (millis() - perfLastFlush < 2000) return;
  perfLastFlush = millis();
  char chunk[sizeof(perfPending)];
  taskENTER_CRITICAL(&perfSpinlock);
  const size_t len = perfPendingLen;
  memcpy(chunk, perfPending, len);
  perfPendingLen = 0;
  taskEXIT_CRITICAL(&perfSpinlock);
  if (!len) return;
  HalFile file = Storage.open(kPerfLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || !file.isOpen()) return;
  file.write(reinterpret_cast<const uint8_t*>(chunk), len);
  file.flush();
  file.close();
}
}  // namespace
#endif

extern HalGPIO gpio;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
#if CROSSPOINT_VECTOR_FONTS || FREEINK_DEVICE_EEGO_A4
  // FreeType rasterization runs on this task, and the deepest observed chain
  // is a glyph fault DURING LAYOUT: expat + parser + line-layout frames
  // (~3.5KB on Xtensa) with the scan converter's FT_RENDER_POOL_SIZE (4KB)
  // stack-resident band pool on top — a measured ~8KB peak that trips the
  // canary on an 8KB stack. Vector-font boards all have PSRAM-class RAM.
  constexpr uint32_t renderTaskStackBytes = 16384;
#else
  constexpr uint32_t renderTaskStackBytes = 8192;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          renderTaskStackBytes,  // Stack size
                          this,                  // Parameters
                          1,                     // Priority
                          &renderTaskHandle,     // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
namespace {
// Keep the previous B/W + delayed-gray candidate available for A/B builds.
// The direct-gray experiment skips both the B/W submission and this idle redraw.
constexpr uint32_t kGraySettleDelayMs = ricky::ds::refresh::kGraySettleDelayMs;

bool graySettleAllowed() { return !ricky::ds::refresh::kDirectGrayUi; }
}  // namespace
#endif

void ActivityManager::renderTaskLoop() {
  auto waitTicks = portMAX_DELAY;
  auto idleDelayTicks = portMAX_DELAY;
  uint32_t idleGeneration = 0;
  [[maybe_unused]] bool graySettlePending = false;
  while (true) {
    const bool idleArmed = waitTicks != portMAX_DELAY;
    const bool foreground = ulTaskNotifyTake(pdTRUE, waitTicks) != 0;
#if FREEINK_READPICO_DIAGNOSTICS && !defined(SIMULATOR)
    const uint32_t queued = foreground ? queuedRenderMs.exchange(0) : 0;
    const uint32_t wokeMs = millis();
#endif
    waitTicks = portMAX_DELAY;
    // Name why an armed idle pass did not run; a reader page cache that never fills
    // looks the same from outside whichever of these fired.
    if (idleArmed && foreground) {
      LOG_DBG("ACT", "Idle render: preempted by an update");
#if RICKYOS_PLUS_READER_TIMING
      LOG_INF("ReaderCache", "idle preempted by an update");
#endif
    } else if (idleArmed && idleRenderCancelled(idleGeneration)) {
      const uint32_t generation = idleRenderGeneration.load(std::memory_order_relaxed);
      const bool byInput = idleGeneration != generation;
      LOG_DBG("ACT", "Idle render: cancelled (%s)",
              byInput ? lastIdleCancel.load(std::memory_order_relaxed)
                      : (isSwitchPending() ? "switch pending" : "update requested"));
#if RICKYOS_PLUS_READER_TIMING
      LOG_INF("ReaderCache", "idle cancelled before start (%s)",
              byInput ? lastIdleCancel.load(std::memory_order_relaxed)
                      : (isSwitchPending() ? "switch pending" : "update requested"));
#endif
      // One idle attempt per render, but a page turn's own touch-up or key release lands
      // during that render and used to spend the attempt: on device the reader's page
      // cache hit about 1 turn in 40. Input that queues no render re-arms the wait, so
      // the pass runs once input has been quiet for the delay. Anything that does render
      // (a turn, a switch, an update) arrives as a foreground wake instead.
      if (byInput && lastIdleCancelRearms.load(std::memory_order_relaxed) && !isSwitchPending() &&
          !requestedUpdate.load()) {
        idleGeneration = generation;
        waitTicks = idleDelayTicks;
        continue;
      }
    }
    RenderLock lock;
#if FREEINK_READPICO_DIAGNOSTICS && !defined(SIMULATOR)
    if (foreground) {
      LOG_INF("PERF", "render wake queue=%u ms lock=%u ms requested=%d pending=%d",
              queued ? static_cast<uint32_t>(wokeMs - (queued - 1)) : 0, static_cast<uint32_t>(millis() - wokeMs),
              queued != 0, isSwitchPending());
    }
#endif
    if (currentActivity && pendingAction.load() == PendingAction::None) {
      HalPowerManager::Lock powerLock;
      if (foreground) {
        // Some renderers release the lock, so read activity-owned metadata
        // before calling them. Input during the render also cancels its idle pass.
        auto delayMs = currentActivity->idleRenderDelayMs();
        [[maybe_unused]] bool directGray = false;
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
        directGray = delayMs == 0 && currentActivity->wantsGraySettle() && ricky::ds::refresh::kDirectGrayUi;
        graySettlePending = delayMs == 0 && currentActivity->wantsGraySettle() && graySettleAllowed();
        if (graySettlePending) delayMs = kGraySettleDelayMs;
#endif
        idleGeneration = idleRenderGeneration.load(std::memory_order_relaxed);
        display.setInverted(SETTINGS.screenInverted != 0);
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
        // A missing gray buffer or failed commit gets the existing readable B/W
        // fallback. The direct path never arms a second, delayed submission.
        if (!directGray || !renderDirectGray(std::move(lock))) {
          mainTabMotion_.invalidate();
          currentActivity->render(std::move(lock));
        }
#else
        currentActivity->render(std::move(lock));
#endif
        idleDelayTicks = delayMs != 0 ? pdMS_TO_TICKS(delayMs) : portMAX_DELAY;
        waitTicks = idleDelayTicks;
      } else if (!idleRenderCancelled(idleGeneration)) {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
        if (graySettlePending) {
          graySettlePending = false;
          settleToGray(std::move(lock), idleGeneration);
        } else {
          currentActivity->renderIdle(idleGeneration);
#if FREEINK_DEVICE_METALIO_PLUS
          if (!idleRenderCancelled(idleGeneration)) currentActivity->renderIdleWarmup(idleGeneration);
#endif
        }
#else
        currentActivity->renderIdle(idleGeneration);
#endif
      }
    }
    // An idle timeout must never acknowledge requestUpdateAndWait(): its
    // notification may have arrived while the idle callback was cancelling.
    if (!foreground) {
#if FREEINK_DEVICE_METALIO_PLUS
      // Input can arrive during renderIdle(), not just during its timeout.
      // A touch-up without a queued redraw must leave interrupted prefetch
      // retryable after a new quiet window. Do not retry successful passes,
      // navigation, queued updates or a disabled cache (delay == 0).
      const uint32_t generation = idleRenderGeneration.load(std::memory_order_relaxed);
      if (currentActivity && generation != idleGeneration &&
          lastIdleCancelRearms.load(std::memory_order_relaxed) && !isSwitchPending() && !requestedUpdate.load()) {
        const auto delayMs = currentActivity->idleRenderDelayMs();
        if (delayMs != 0) {
          idleGeneration = generation;
          idleDelayTicks = pdMS_TO_TICKS(delayMs);
          waitTicks = idleDelayTicks;
          LOG_DBG("ACT", "Idle render: retry after interrupted pass");
#if RICKYOS_PLUS_READER_TIMING
          LOG_INF("ReaderCache", "idle interrupted (%s), retry", lastIdleCancel.load(std::memory_order_relaxed));
#endif
        }
      }
#endif
      continue;
    }
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
bool ActivityManager::renderDirectGray(RenderLock&& lock) {
  if (!renderer.beginGrayscale16()) {
    mainTabMotion_.invalidate();
    LOG_ERR("ACT", "Direct gray: unavailable, B/W fallback");
    return false;
  }
  const unsigned long started = millis();
#if RICKYOS_PLUS_TAB_BENCH
  LOG_INF("TABBENCH", "render_start %s t=%lu", currentActivity->name.c_str(), started);
#endif
  const uint32_t renderGeneration = renderRequestGeneration.load(std::memory_order_relaxed);
  // Renderers may release the lock; capture page-owned geometry first.
  const bool mainTab = currentActivity->usesMainTabBar();
  const MainTabLayout layout = mainTab ? currentActivity->mainTabLayout() : MainTabLayout{Rect{}, Rect{}, Rect{}};
  const Rect navigation = layout.tabBar;
  const Rect content = layout.content;
  const int tabIndex = mainTab ? MainTabs::indexOf(currentActivity->mainTab()) : -1;
  const auto orientation = static_cast<uint8_t>(renderer.getOrientation());
  renderer.setDisplayCapture(true);
  currentActivity->render(std::move(lock));
  renderer.setDisplayCapture(false);
  if (!renderer.isGrayscale16Active()) {
    mainTabMotion_.invalidate();
    return true;
  }
  // A plain touch/release must not discard the only foreground frame. Discard
  // only when a newer redraw or navigation is queued (e.g. a cover finishes).
  if (isSwitchPending() || requestedUpdate.load() ||
      renderGeneration != renderRequestGeneration.load(std::memory_order_relaxed)) {
    renderer.cancelGrayscale16();
    LOG_DBG("ACT", "Direct gray: superseded");
    return true;
  }
#if !defined(SIMULATOR)
  const bool adaptiveTabRefresh = mainTab && RICKYOS_MAIN_TAB_MOTION_ENABLED && SETTINGS.mainTabMotion != 0;
  renderer.setNextGray16UiRefresh(navigation.x, navigation.y, navigation.width, navigation.height,
                                  ricky::ds::refresh::kGc16EveryDirectFrames,
                                  ricky::ds::refresh::kGc16EveryControlFrames, adaptiveTabRefresh);
  if (renderer.nextRefreshNeedsCleanup()) display.setNextGray16Profile(2);
  const int direction = mainTabMotion_.directionTo(tabIndex, orientation, content,
      RICKYOS_MAIN_TAB_MOTION_ENABLED && SETTINGS.mainTabMotion != 0, renderer.nextRefreshNeedsCleanup());
  if (direction) renderer.setNextMainTabTurn(content.x, content.y, content.width, content.height, direction > 0);
#else
  (void)navigation;
#endif
  const unsigned long painted = millis();
  const bool committed = renderer.commitGrayscale16();
  mainTabMotion_.presented(tabIndex, orientation, content, committed);
  if (!committed) LOG_ERR("ACT", "Direct gray commit failed; B/W fallback");
  if (committed) renderer.clearNextRefreshOverride();
  LOG_DBG("ACT", "Direct gray: %s paint=%lu ms submit=%lu ms total=%lu ms", committed ? "done" : "failed, B/W fallback",
          painted - started, millis() - painted, millis() - started);
#if RICKYOS_PLUS_TAB_BENCH
  LOG_INF("TABBENCH", "painted %s paint=%lu submit=%lu end=%lu", currentActivity ? currentActivity->name.c_str() : "?",
          painted - started, millis() - painted, millis());
#endif
#if RICKYOS_PERF_SD_LOG
  {
    char line[256];
    int n = snprintf(line, sizeof(line), "%lu %s paint=%lu submit=%lu%s", static_cast<unsigned long>(started),
                     currentActivity ? currentActivity->name.c_str() : "?", painted - started, millis() - painted,
                     committed ? "" : " failed");
#if RICKYOS_PAINT_PROFILE
    const auto& profile = GfxRenderer::paintProfile();
    if (n > 0 && n < static_cast<int>(sizeof(line)))
      n += snprintf(line + n, sizeof(line) - n, " fill=%lu/%lu text=%lu/%lu shape=%lu/%lu image=%lu/%lu",
                    profile.us[0] / 1000, profile.calls[0], profile.us[1] / 1000, profile.calls[1],
                    profile.us[2] / 1000, profile.calls[2], profile.us[3] / 1000, profile.calls[3]);
#endif
    if (n > 0 && n < static_cast<int>(sizeof(line)) - 1) {
      line[n] = '\n';
      line[n + 1] = '\0';
      queuePerfLine(line);
    }
  }
#endif
#if RICKYOS_PAINT_PROFILE
  {
    auto& profile = GfxRenderer::paintProfile();
    LOG_INF("PERF", "paint %s: fill=%lums/%lu text=%lums/%lu shape=%lums/%lu image=%lums/%lu",
            currentActivity ? currentActivity->name.c_str() : "?", profile.us[0] / 1000, profile.calls[0],
            profile.us[1] / 1000, profile.calls[1], profile.us[2] / 1000, profile.calls[2], profile.us[3] / 1000,
            profile.calls[3]);
    profile = {};
  }
#endif
#if FREEINK_READPICO_DIAGNOSTICS && !defined(SIMULATOR)
  const auto internal = HalMemory::getInternalHeap();
  const auto dma = HalMemory::getInternalDmaHeap();
  const auto psram = HalMemory::getPsramHeap();
  LOG_INF("PERF", "gray heap internal=%u/%u dma=%u/%u psram=%u/%u (free/maxBlock)",
          static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.largestBlockBytes),
          static_cast<unsigned>(dma.freeBytes), static_cast<unsigned>(dma.largestBlockBytes),
          static_cast<unsigned>(psram.freeBytes), static_cast<unsigned>(psram.largestBlockBytes));
#endif
  return committed;
}

void ActivityManager::settleToGray(RenderLock&& lock, const uint32_t generation) {
  if (!renderer.beginGrayscale16()) return;
  const unsigned long started = millis();
  renderer.setDisplayCapture(true);
  currentActivity->render(std::move(lock));
  renderer.setDisplayCapture(false);
  if (!renderer.isGrayscale16Active()) return;
  if (idleRenderCancelled(generation)) {
    // Input arrived while drawing: the B/W frame on the glass is still current.
    renderer.cancelGrayscale16();
    LOG_DBG("ACT", "Gray settle: cancelled by input");
    return;
  }
#if !defined(SIMULATOR)
  // The text-turn waveform only drives pixels it believes changed, so small errors in
  // the panel's real state build up across fast refreshes and settles (old screens show
  // through). Every few settles is a full GC16 clean instead.
  static uint8_t settles = 0;
  constexpr uint8_t kCleanEvery = ricky::ds::refresh::kGc16EverySettles;
  settles = static_cast<uint8_t>((settles + 1) % kCleanEvery);
  display.setNextGray16Profile(settles == 0 ? 2 : 1);
#endif
  const bool committed = renderer.commitGrayscale16();
  LOG_DBG("ACT", "Gray settle: %s in %lu ms", committed ? "done" : "failed", millis() - started);
}
#endif

void ActivityManager::trackInputMode() {
  if (!mappedInput.hasTouch()) return;
  if (mappedInput.wasAnyPressed()) {
    if (!keyFocusShown) {
      keyFocusShown = true;
      requestUpdate();
    }
  } else if (keyFocusShown && mappedInput.wasScreenTouchReleased()) {
    keyFocusShown = false;
    requestUpdate();  // a touch on empty space produces no frame of its own
  }
}

void ActivityManager::loop() {
#if RICKYOS_PERF_SD_LOG
  // Never write while the card is exported over USB.
  if (!currentActivity || currentActivity->name != "UsbDrive") flushPerfLog();
#endif
#if FREEINK_DEVICE_READPICO && !defined(SIMULATOR)
  {
    // Page turns and UI frames leave the panel rails up so the next frame starts at once;
    // held up while nothing is driven, they let charge build in the film until old pages
    // show through new ones. Drop them once frames have stopped for a few seconds (the
    // PMIC's own 500 ms off-hold falls in the idle time). Never during a frame push.
    RenderLock railsLock(RenderLock::Mode::Try);
    if (railsLock.ownsLock()) display.railsOffIfIdle(3000);
  }
#endif
  if (mappedInput.consumeSuppressedRelease()) {
    resetHomeStandbyInput();
    return;
  }
#if RICKYOS_PLUS_SCREEN_TOUR == 2
  {
    // Diagnostic: screenshots of the user's own books (port/PlusScreenTour2.cpp keeps and
    // restores the state they touch). 0: home as "Ricky", 1: 理想国 in 古籍 at the chapter
    // with 玻勒马霍斯's line, 2: 舞！舞！舞！ in 小狗伴读, 3: Standby with the 破纸而出 sticker.
    static bool checked = false;
    static unsigned step = 0;
    static uint32_t nextAt = 25000;
    static char nickname[sizeof(SETTINGS.rickyNickname)];
    if (!checked) {
      checked = true;
      if (plusTour2Restore()) {
        delay(500);
        esp_restart();
      }
      if (plusTour2Finished()) step = 99;
    }
    if (step < 4 && currentActivity && currentActivity->name != "Boot" && millis() >= nextAt) {
      LOG_INF("TOUR2", "step=%u from=%s", step, currentActivity->name.c_str());
      nextAt = millis() + 45000;
      const std::string republic = plusTour2Book("理想国"), dance = plusTour2Book("舞！舞！舞！");
      switch (step++) {
        case 0:
          if (!plusTour2Backup(republic, dance)) {
            LOG_ERR("TOUR2", "backup failed, tour stopped");
            step = 99;
            return;
          }
          memcpy(nickname, SETTINGS.rickyNickname, sizeof(nickname));
#if RICKYOS_PLUS_SCREEN_TOUR_FROM > 0
          // Re-shoot from a later step; the backup above still covers every step.
          step = RICKYOS_PLUS_SCREEN_TOUR_FROM;
          nextAt = millis() + 5000;
          return;
#endif
          strncpy(SETTINGS.rickyNickname, "Ricky", sizeof(SETTINGS.rickyNickname) - 1);
          goToInxRecent();
          return;
        case 1: {
          memcpy(SETTINGS.rickyNickname, nickname, sizeof(nickname));
          const int spine = republic.empty() ? -1 : plusTour2FindChapter(republic, "人多势众");
          LOG_INF("TOUR2", "republic=%s spine=%d", republic.c_str(), spine);
          if (spine >= 0) plusTour2SetProgress(republic, spine);
          SETTINGS.readingMode = static_cast<uint8_t>(readingMode::Mode::Ancient);
          if (!republic.empty()) goToReader(republic);
          return;
        }
        case 2:
          SETTINGS.readingMode = static_cast<uint8_t>(readingMode::Mode::Puppy);
          LOG_INF("TOUR2", "dance=%s", dance.c_str());
          if (!dance.empty()) goToReader(dance);
          return;
        case 3:
          LOG_INF("TOUR2", "sticker=%d", plusTour2InstallSticker());
          SETTINGS.rickyStandbyFace = CrossPointSettings::RICKY_STANDBY_TRANSPARENT;
          goToStandby();
          return;
      }
    }
  }
#elif RICKYOS_PLUS_SCREEN_TOUR
  {
    // Diagnostic: visit the screens used for screenshots, one every 10 s from 25 s after boot.
    static unsigned step = RICKYOS_PLUS_SCREEN_TOUR_FROM;
    static uint32_t nextAt = 25000;
    if (step < RICKYOS_PLUS_SCREEN_TOUR_TO && currentActivity && currentActivity->name != "Boot" && millis() >= nextAt) {
      LOG_INF("TOUR", "step=%u from=%s", step, currentActivity->name.c_str());
      nextAt = millis() + 30000;
      switch (step++) {
        case 0: goToLibrary(); return;
        case 1: goToSettings(); return;
        case 2: goToInxRecent(); return;
        case 3:
          if (!RECENT_BOOKS.getBooks().empty()) goToReader(RECENT_BOOKS.getBooks().front().path);
          return;
        case 5: goToInxRecent(); return;
        default: {  // 4: control center over the book, 6: over home
          auto panel = makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput,
                                                                  currentActivity->name == "EpubReader");
          if (panel) pushActivity(std::move(panel));
          return;
        }
      }
    }
  }
#endif
#if RICKYOS_PLUS_TAB_BENCH
  {
    // Diagnostic: walk the tab bar unattended, three rounds, 5 s apart from 25 s after boot.
    static uint32_t nextAt = 25000;
    static unsigned step = 0;
    if (step < 3 * MainTabs::values.size() && currentActivity && currentActivity->usesMainTabBar() &&
        millis() >= nextAt) {
      const MainTab target = MainTabs::values[(step + 1) % MainTabs::values.size()];
      LOG_INF("TABBENCH", "step=%u from=%d to=%d t=%llu heap=%u psram=%u", step,
              static_cast<int>(currentActivity->mainTab()), static_cast<int>(target),
              static_cast<unsigned long long>(esp_timer_get_time() / 1000), (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getFreePsram());
      ++step;
      nextAt = millis() + 5000;
      goToMainTab(target);
      return;
    }
  }
#endif

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false) && renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  trackInputMode();

  if (currentActivity && pendingAction.load() == PendingAction::None) {
    if (handleMainTabInput()) {
      resetHomeStandbyInput();
      return;
    }
    // Standby is the screen at rest: only the side key (or Back) leaves it. Edge swipes
    // must not pull the control center or Home over it.
    const bool atRest = standbyShowing();
    if (!atRest && !currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      resetHomeStandbyInput();
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Touch users can also open the global control center from the status bar.
    bool statusBarTap = false;
    const bool statusTapOpensPanel = SETTINGS.rickyGestureStatus != 0;  // Settings -> Gestures
    if (mappedInput.hasTouch() && statusTapOpensPanel) {
      int tx = 0;
      int ty = 0;
      if (currentActivity->usesMainTabBar()) {
        const Rect status = currentActivity->mainTabLayout().statusBar;
        statusBarTap = mappedInput.wasScreenTapped(tx, ty) && tx >= status.x && tx < status.x + status.width &&
                       ty >= status.y && ty < status.y + status.height;
      } else if (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
                 currentActivity->name == "Settings" || currentActivity->name == "NetworkModeSelection") {
        statusBarTap =
            mappedInput.wasScreenTapped(tx, ty) && ty >= 0 && ty < 44 && !HeaderBackTapTarget::contains(tx, ty);
      }
    }
    if (!atRest && currentActivity->name != "FrontlightPanel" && (statusBarTap || mappedInput.wasLightPanelGesture())) {
      resetHomeStandbyInput();
      // Over a book the panel adds its reading controls.
      const bool overReader = currentActivity->name == "EpubReader";
      auto panel = makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput, overReader);
      if (!panel) {
        LOG_ERR("ACT", "OOM: frontlight panel (%u bytes)", static_cast<unsigned>(sizeof(FrontlightPanelActivity)));
        return;
      }
      pushActivity(std::move(panel));
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    if (!handleHomeStandbyInput()) currentActivity->loop();
  }

  while (pendingAction.load() != PendingAction::None) {
    if (pendingAction.load() == PendingAction::Pop) {
      if (RenderLock::peek()) break;
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction.store(PendingAction::None);
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction.store(PendingAction::None);

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        resetHomeStandbyInput();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction.load() == PendingAction::None) {
          LOG_DBG("ACT", "Resuming activity: %s", currentActivity->name.c_str());
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      if (RenderLock::peek()) break;
      RenderLock lock;

      if (pendingAction.load() == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction.load() == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        // The parent's header back rect must not route taps on the pushed
        // screen (which may draw no header of its own).
        HeaderBackTapTarget::clear();
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction.store(PendingAction::None);
      currentActivity = std::move(pendingActivity);
      resetHomeStandbyInput();

      // Drop any one-shot tap/release edge events the outgoing activity already
      // consumed this frame. The SDK's InputManager clears these in update(),
      // but a pushActivity runs mid-frame; without this, the incoming activity
      // re-reads the same tap and double-activates (observed crash with WeRead:
      // the second activation hit WiFi/render-lock interleaving and tripped
      // FreeRTOS xTaskPriorityDisinherit on the rendering mutex).
#if !defined(SIMULATOR)
      gpio.clearTouchTapEvent();
#endif

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (pendingAction.load() == PendingAction::None && requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::resetHomeStandbyInput() {
  standbyBackState = mappedInput.isPressed(MappedInputManager::Button::Back) ? StandbyBackState::WaitingForRelease
                                                                             : StandbyBackState::Idle;
}

bool ActivityManager::handleHomeStandbyInput() {
  const bool eligible =
      currentActivity && (currentActivity->isHomeActivity() ||
                          (currentActivity->usesMainTabBar() && currentActivity->mainTab() == MainTab::Recent &&
                           mainTabFocus == MainTabFocus::Tabs));
  // With a Home key (Plus), Back on Home stays Back: the power key owns Standby there.
  if (!eligible || !SETTINGS.standbyShortcutEnabled || BoardConfig::hasHomeKey()) {
    resetHomeStandbyInput();
    return false;
  }

  const bool pressed = mappedInput.wasPressed(MappedInputManager::Button::Back);
  const bool released = mappedInput.wasReleased(MappedInputManager::Button::Back);
  // Standby is never a touch gesture on Home: a rightward swipe from the left quarter
  // of the screen counts as Back, and turning the 最近翻过 page from its left book put
  // the device into Standby, whose brand screen reads as a reboot. Standby stays on
  // its own entries (Apps -> Standby, the physical keys).
  if (mappedInput.hasTouch() && mappedInput.wasBackGesture()) {
    resetHomeStandbyInput();
    return false;
  }
  // Touch Back gestures publish a complete pair in one frame, independent of
  // an inherited physical hold. They remain usable through the release barrier.
  if (pressed && released) {
    standbyBackState = StandbyBackState::Idle;
    goToStandby();
    return true;
  }

  switch (standbyBackState) {
    case StandbyBackState::Idle:
      if (pressed) standbyBackState = StandbyBackState::Pressed;
      break;
    case StandbyBackState::Pressed:
      if (released) {
        standbyBackState = StandbyBackState::Idle;
        goToStandby();
        return true;
      }
      if (!mappedInput.isPressed(MappedInputManager::Button::Back)) standbyBackState = StandbyBackState::Idle;
      break;
    case StandbyBackState::WaitingForRelease:
      if (!mappedInput.isPressed(MappedInputManager::Button::Back)) standbyBackState = StandbyBackState::Idle;
      break;
  }
  // Home Activities no longer handle Back themselves; ignored releases must
  // still allow independent touch input, including on the release frame.
  return false;
}

bool ActivityManager::handleMainTabInput() {
  if (!currentActivity || !currentActivity->usesMainTabBar()) return false;

  const MainTab currentTab = currentActivity->mainTab();
  const Rect tabBar = currentActivity->mainTabLayout().tabBar;
  const auto insideTabs = [&tabBar](const int x, const int y) {
    return x >= tabBar.x && x < tabBar.x + tabBar.width && y >= tabBar.y && y < tabBar.y + tabBar.height;
  };

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (insideTabs(x, y)) {
      const MainTab target = MainTabs::fromX(x - tabBar.x, tabBar.width);
      if (target != MainTab::None) {
        mainTabFocus = MainTabFocus::Content;
        if (target != currentTab)
          goToMainTab(target);
        else
          requestUpdate();
      }
      return true;
    }
    if (mainTabFocus == MainTabFocus::Tabs) {
      mainTabFocus = MainTabFocus::Content;
      requestUpdate();
    }
    return false;
  }

  if (mainTabFocus == MainTabFocus::Tabs && mappedInput.wasScreenTouchDown(x, y) && !insideTabs(x, y)) {
    mainTabFocus = MainTabFocus::Content;
    requestUpdate();
    return false;
  }

  if (mainTabEntryReleasePending) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down))
      mainTabEntryReleasePending = false;
    return true;
  }

  switch (mainTabFocus) {
    case MainTabFocus::Tabs:
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        goToMainTab(MainTabs::adjacent(currentTab, -1));
        return true;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        goToMainTab(MainTabs::adjacent(currentTab, 1));
        return true;
      }
      if (mappedInput.isPressed(MappedInputManager::Button::Left) ||
          mappedInput.isPressed(MappedInputManager::Button::Right)) {
        return true;
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        mainTabFocus = MainTabFocus::Content;
        requestUpdate();
        return true;
      }
      if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) return true;

      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        mainTabFocus = MainTabFocus::Content;
        mainTabEntryReleasePending = true;
        currentActivity->selectMainTabContentEdge(MainTabContentEdge::First);
        requestUpdate();
        return true;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        mainTabFocus = MainTabFocus::Content;
        mainTabEntryReleasePending = true;
        currentActivity->selectMainTabContentEdge(MainTabContentEdge::Last);
        requestUpdate();
        return true;
      }

      // Recent's Back is owned by the shared home Standby handler.
      if (currentTab == MainTab::Recent) return false;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        goToMainTab(MainTabs::backTarget(currentTab));
        return true;
      }
      return mappedInput.isPressed(MappedInputManager::Button::Back);

    case MainTabFocus::Content:
      if (!currentActivity->mainTabBackReturnsToTabs()) return false;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        mainTabFocus = MainTabFocus::Tabs;
        requestUpdate();
        return true;
      }
      return mappedInput.isPressed(MappedInputManager::Button::Back);
  }
  return false;
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
  // The outgoing screen's header back button must not eat taps on the next
  // screen; the next header draw re-records it.
  HeaderBackTapTarget::clear();
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  cancelIdleRender();
  mappedInput.resetHomeButtonInput();
  standbyBackState = StandbyBackState::Idle;
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    resetHomeStandbyInput();
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() { replaceActivityWith<CrossPointWebServerActivity>(); }

void ActivityManager::goToJoinNetwork() {
  // Post heap-defrag reboot: enter the web-server activity straight in Join
  // Network mode (skips mode selection, does not reboot again).
  replaceActivityWith<CrossPointWebServerActivity>(/*startInJoinNetwork=*/true);
}

void ActivityManager::goToUsbDrive() {
#if FREEINK_CAP_USB_MSC || defined(RICKYOS_USB_PREVIEW)
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

void ActivityManager::goToSettings() { replaceActivityWith<SettingsActivity>(); }

void ActivityManager::goToFileBrowser(std::string path) { replaceActivityWith<FileBrowserActivity>(std::move(path)); }

void ActivityManager::goToRecentBooks() { replaceActivityWith<RecentBooksActivity>(); }

void ActivityManager::goToInxRecent() { replaceActivityWith<InxRecentActivity>(); }

void ActivityManager::goToMainTab(const MainTab tab) {
  mainTabEntryReleasePending = false;
  switch (tab) {
    case MainTab::Recent:
      goToInxRecent();
      return;
    case MainTab::Library:
      goToLibrary();
      return;
    case MainTab::StorageFiles:
      replaceActivityWith<RickyStorageActivity>();
      return;
    case MainTab::Settings:
      goToSettings();
      return;
    case MainTab::Statistics:
      goToReadingStats();
      return;
    case MainTab::Apps:
      goToApps();
      return;
    case MainTab::None:
      return;
  }
}

void ActivityManager::goToLibrary() {
  auto activity = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: library activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }
  if (FsHelpers::hasImageExtension(path)) {
    replaceActivityWith<ImageViewerActivity>(std::move(path));
    return;
  }
  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (activity) replaceActivity(std::move(activity));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  if (replaceActivityWith<SleepActivity>(fromTimeout)) {
    loop();  // The caller sleeps immediately after this returns, so render now.
  }
}

void ActivityManager::goToBoot() { replaceActivityWith<BootActivity>(); }

bool ActivityManager::goToPostOtaBoot(bool allowAutoPreload) {
  return replaceActivityWith<BootActivity>(BootActivity::Mode::PostOta, allowAutoPreload);
}

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivityWith<FullScreenMessageActivity>(std::move(message), style);
}

void ActivityManager::goHome(HomeMenuItem) {
  if (!CrossPointSettings::requiresOnboarding(SETTINGS.onboardingVersion)) {
    UserGuide::installIfPending(static_cast<Language>(SETTINGS.language) == Language::ZH_CN);
  }
  // The theme is pinned to INX (CrossPointSettings::enforceProductLayout): Home is the
  // main-tab Recent page.
  mainTabFocus = MainTabFocus::Tabs;
  mainTabEntryReleasePending = false;
  goToInxRecent();
}
void ActivityManager::goToCrashReport() { replaceActivityWith<CrashActivity>(); }

void ActivityManager::goToApps() { replaceActivityWith<AppsMenuActivity>(); }

void ActivityManager::goToReadingStatsMenu() { goToRickyReadingStats(); }
void ActivityManager::goToRickyReadingStats() { replaceActivityWith<RickyReadingStatsActivity>(); }

void ActivityManager::goToReadingStats() { goToRickyReadingStats(); }

void ActivityManager::goToGomoku() { replaceActivityWith<GomokuModeActivity>(); }

void ActivityManager::goToCalculator() { replaceActivityWith<CalculatorActivity>(); }
#if FREEINK_DEVICE_METALIO_PLUS
void ActivityManager::goToRecorder() { replaceActivityWith<RecorderActivity>(); }
#endif

void ActivityManager::goToStandby() { replaceActivityWith<StandbyActivity>(); }

bool ActivityManager::standbyShowing() const { return currentActivity && currentActivity->name == "Standby"; }

void ActivityManager::openStandby() {
  if (standbyShowing() || isSwitchPending()) return;
  auto standby = makeUniqueNoThrow<StandbyActivity>(renderer, mappedInput);
  if (!standby) {
    LOG_ERR("ACT", "OOM: standby");
    return;
  }
  pushActivity(std::move(standby));
}

void ActivityManager::closeStandby() {
  if (!standbyShowing() || isSwitchPending()) return;
  popActivity();  // back to the page under it; Home when Standby was the only one
}
void ActivityManager::goToStandbySettings() { replaceActivityWith<RickyStandbySettingsActivity>(); }
void ActivityManager::goToGallery() {
  replaceActivityWith<StandbyGalleryActivity>(StandbyPictures::Target::Standby, StandbyGalleryActivity::Mode::Browse);
}

void ActivityManager::goToWeRead() { replaceActivityWith<WeReadActivity>(); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  cancelIdleRender();
  mappedInput.resetHomeButtonInput();
  standbyBackState = StandbyBackState::Idle;
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  cancelIdleRender();
  mappedInput.resetHomeButtonInput();
  standbyBackState = StandbyBackState::Idle;
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::keepsBluetoothAlive() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->keepsBluetoothAlive(); }) ||
         (currentActivity && currentActivity->keepsBluetoothAlive());
}

bool ActivityManager::deferBluetoothStart() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->deferBluetoothStart(); }) ||
         (currentActivity && currentActivity->deferBluetoothStart());
}

bool ActivityManager::handleForcedRefresh() {
  cancelIdleRender();
  if (!currentActivity) return false;
  if (currentActivity->handleForcedRefresh()) return true;
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
  if (ricky::ds::refresh::kDirectGrayUi && currentActivity->wantsGraySettle() &&
      currentActivity->idleRenderDelayMs() == 0) {
    // Refresh the real gray UI, not its approximate 1-bit proxy.
    renderer.requestNextFullRefresh();
    requestUpdate();
    return true;
  }
#endif
  return false;
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::prepareForSleep() {
  RenderLock lock;
  for (const auto& activity : stackActivities) activity->prepareForSleep();
  if (currentActivity) currentActivity->prepareForSleep();
}

#if FREEINK_READPICO_DIAGNOSTICS && !defined(SIMULATOR)
void ActivityManager::recordRenderRequest() {
  uint32_t empty = 0;
  queuedRenderMs.compare_exchange_strong(empty, static_cast<uint32_t>(millis()) + 1);
}
#endif

void ActivityManager::requestUpdate(bool immediate) {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
  renderRequestGeneration.fetch_add(1, std::memory_order_relaxed);
#endif
#if FREEINK_READPICO_DIAGNOSTICS && !defined(SIMULATOR)
  recordRenderRequest();
#endif
  cancelIdleRender();
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
  renderRequestGeneration.fetch_add(1, std::memory_order_relaxed);
#endif
#if FREEINK_READPICO_DIAGNOSTICS && !defined(SIMULATOR)
  recordRenderRequest();
#endif
  cancelIdleRender();
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  bool holdingRenderLock = (xSemaphoreGetMutexHolder(renderingMutex) == currTaskHandler);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  // This render draws the current state, so it also serves a deferred requestUpdate()
  // made earlier in this loop pass. Left set, that flag marks every frame of a long
  // synchronous job (font acceleration: two minutes) as superseded, and the direct-gray
  // path discards them all: the progress screen never appeared.
  requestedUpdate = false;
  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock(Mode mode) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, mode == Mode::Try ? 0 : portMAX_DELAY) == pdTRUE;
  assert((mode == Mode::Try || isLocked) && "Blocking render lock acquisition failed");
}

RenderLock::RenderLock(Activity&) : RenderLock(Mode::Blocking) {}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
