#include <HalAudioOutput.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <BleInput.h>
#include <HalRecorder.h>
#endif
#include "StandbyActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>

#if CROSSPOINT_EMULATED == 0
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#endif

#include <algorithm>
#include <string>

#include "CalendarClockFace.h"
#include "ChineseCalendarFace.h"
#include "CrossPointSettings.h"
#include "KeepPageFace.h"
#include "NetworkStartup.h"
#include "ReadingStatsStore.h"
#include "RickyWallpaperDownloadActivity.h"
#include "StandbyGalleryActivity.h"
#include "SloppyClockFace.h"
#include "StandbyDiag.h"
#include "StandbyTime.h"
#include "WallpaperFace.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PaginationDots.h"
#include "util/TimeUtils.h"

namespace {

constexpr uint32_t kWifiTimeoutMs = 15000u;  // Same as WifiSelectionActivity
constexpr uint32_t kNtpTimeoutMs = 12000u;
constexpr uint32_t kSyncDelayMs = 1500u;
#if CROSSPOINT_EMULATED == 0
[[maybe_unused]] constexpr uint32_t kMaxLightSleepSeconds = 60u;
#endif

// Face factory table. Add new faces by appending a row here and including the
// corresponding header above. Each entry also declares an isAvailable()
// predicate so faces can be hidden under specific runtime conditions (e.g.
// orientation-only). When a face is unavailable it is skipped during cycling
// and the bottom pager-dot strip collapses accordingly.
struct FaceEntry {
  std::unique_ptr<StandbyFace> (*create)();
  bool (*isAvailable)(int sw, int sh);
};
constexpr FaceEntry kFaces[] = {
    // RickyOS Standby is the picture or the book's cover, the desk calendar clock, or
    // the page left as it was (rickyStandbyFace); the stock clock faces are not offered.
    {[]() -> std::unique_ptr<StandbyFace> {
       switch (SETTINGS.rickyStandbyFace) {
         case CrossPointSettings::RICKY_STANDBY_CALENDAR:
           return makeUniqueNoThrow<CalendarClockFace>();
         case CrossPointSettings::RICKY_STANDBY_KEEP_PAGE:
           return makeUniqueNoThrow<KeepPageFace>();
         case CrossPointSettings::RICKY_STANDBY_TRANSPARENT:
           return makeUniqueNoThrow<KeepPageFace>(true);
         default:
           return makeUniqueNoThrow<WallpaperFace>();  // the picture, or the cover
       }
     },
     [](int, int) { return true; }},
};
constexpr uint8_t kFaceCount = static_cast<uint8_t>(sizeof(kFaces) / sizeof(kFaces[0]));

uint8_t countAvailableFaces(int sw, int sh) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kFaceCount; ++i) {
    if (kFaces[i].isAvailable(sw, sh)) ++n;
  }
  return n;
}

// Map a "rank among available faces" (0-based) back to a kFaces[] index.
// Returns kFaceCount if none match (only happens when no face is available,
// which can't currently occur since SloppyClockFace is always available).
uint8_t availableFaceIndexFromRank(int sw, int sh, uint8_t rank) {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < kFaceCount; ++i) {
    if (!kFaces[i].isAvailable(sw, sh)) continue;
    if (seen == rank) return i;
    ++seen;
  }
  return kFaceCount;
}

uint8_t rankOfAvailableFace(int sw, int sh, uint8_t faceIdx) {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < kFaceCount; ++i) {
    if (!kFaces[i].isAvailable(sw, sh)) continue;
    if (i == faceIdx) return seen;
    ++seen;
  }
  return 0;  // faceIdx is unavailable — caller treats it as "use the first"
}

// Once per power-on, Standby may push the WiFi selection UI to help the user
// get online for clock sync. After it has been shown (regardless of whether the
// user connected or cancelled), subsequent Standby entries within the same
// session will not re-prompt — the explicit way to retry is Settings → WiFi.
// This module-scope flag persists across activity destroy/recreate.
bool g_promptedForWifiThisSession = false;

// Bottom-center page indicator dots. One dot per face, filled for the current
// face. Drawn only in Normal mode — hidden once we go Immersive. Shares the dot
// style with the Apps menu via drawPaginationDots().
constexpr int kDotBottomInset = 24;  // distance from bottom edge to top of dots

void drawFaceDots(const GfxRenderer& renderer, int sw, int sh, uint8_t total, uint8_t current) {
  drawPaginationDots(renderer, sw, sh - kDotBottomInset, total, current);
}

}  // namespace

void StandbyActivity::onEnter() {
  Activity::onEnter();
#if FREEINK_DEVICE_METALIO_PLUS
  HalRecorder::stop();
  HalAudioOutput::shutdown();
  mappedInput.stopHapticFeedback();
#endif
  savedFrontlightOn_ = Frontlight.present() && Frontlight.isOn();
  Frontlight.setOn(false);
  LOG_INF("STANDBY", "Frontlight suspended: wasOn=%u", savedFrontlightOn_);
#if CROSSPOINT_EMULATED == 0
  HalPowerManager::beginStandbyKeyWatch();
#endif
  // The calendar clock stands on its side, the keys under the screen to the left: the
  // portrait bottom is the panel's right edge, which LandscapeClockwise puts at x = 0
  // (the reader's 横屏（顺时针）). Only Standby turns; the orientation is restored on exit.
  savedOrientation_ = renderer.getOrientation();
  if (SETTINGS.rickyStandbyFace == CrossPointSettings::RICKY_STANDBY_CALENDAR) {
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  }
  LOG_DBG("STANDBY", "onEnter free heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  // Always default to face 0 (Sloppy Clock). If face 0 is somehow unavailable
  // (currently impossible), fall back to the first available index.
  faceIndex_ = 0;
  if (!kFaces[0].isAvailable(renderer.getScreenWidth(), renderer.getScreenHeight())) {
    const uint8_t idx = availableFaceIndexFromRank(renderer.getScreenWidth(), renderer.getScreenHeight(), 0);
    if (idx < kFaceCount) faceIndex_ = idx;
  }
  inverseMode_ = false;
  currentFace_ = kFaces[faceIndex_].create();
  if (!currentFace_) {
    LOG_ERR("STANDBY", "OOM allocating face");
    activityManager.goToApps();
    return;
  }
  currentFace_->onEnter();
  // The picture is the whole screen from the start: no title, battery or face dots.
  mode_ = DisplayMode::Immersive;
  drewFrame_ = false;
  lastInputMs_ = millis();
  enteredMs_ = millis();
  // A face without a clock or date never needs the WiFi sync (nor its prompt).
  if (!TimeUtils::isClockValid() && SETTINGS.clockAutoSync && currentFace_->wantsClock()) {
    syncState_ = SyncState::Delayed;
    syncStartMs_ = millis();
  }
  requestUpdate();
}

void StandbyActivity::onExit() {
  STANDBY_DIAG("standby exit (up %lus)", (millis() - enteredMs_) / 1000);
  switch (syncState_) {
    case SyncState::Idle:
    case SyncState::Delayed:
      break;
    case SyncState::WifiConnecting:
    case SyncState::ClockSyncing:
      stopTimeSyncWifi();
      break;
  }
  syncState_ = SyncState::Idle;
  if (currentFace_) {
    currentFace_->onExit();
    currentFace_.reset();
  }
  // The panel holds a 16-gray picture the B/W proxy only approximates. The next page
  // refreshes differentially against that proxy, so pixels it thinks unchanged would
  // keep the picture's grays under the new page; a full refresh clears them.
  renderer.requestNextRefresh(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(savedOrientation_);
  // Time in Standby is not reading time: the book under it starts its gap afresh.
  READING_STATS.resumeSession();
  const bool restoreLight = savedFrontlightOn_ && SETTINGS.frontlightRestoreOnWake != 0;
  Frontlight.setOn(restoreLight);
  LOG_INF("STANDBY", "Frontlight resumed: on=%u", restoreLight);
  LOG_DBG("STANDBY", "onExit free heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  Activity::onExit();
}

void StandbyActivity::switchFace(int8_t delta) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const uint8_t avail = countAvailableFaces(sw, sh);
  if (avail <= 1) return;

  const uint8_t curRank = rankOfAvailableFace(sw, sh, faceIndex_);
  const uint8_t newRank = static_cast<uint8_t>((curRank + avail + delta) % avail);
  const uint8_t newIdx = availableFaceIndexFromRank(sw, sh, newRank);
  if (newIdx >= kFaceCount || newIdx == faceIndex_) return;

  if (currentFace_) currentFace_->onExit();
  currentFace_.reset();
  faceIndex_ = newIdx;
  currentFace_ = kFaces[faceIndex_].create();
  if (!currentFace_) {
    LOG_ERR("STANDBY", "OOM switching face");
    activityManager.goToApps();
    return;
  }
  currentFace_->onEnter();
  drewFrame_ = false;
  requestUpdate();
}

void StandbyActivity::startTimeSync() {
  if (TimeUtils::isClockValid() || !SETTINGS.clockAutoSync) return;

  // Another activity (e.g. Settings → WiFi) may have already connected the
  // device. Skip our own WiFi.begin in that case and request a clock sync.
  if (WiFi.status() == WL_CONNECTED) {
    NetworkStartup::prepare(renderer);
    LOG_DBG("STANDBY", "WiFi already connected, skipping silent attempt");
    beginClockSync();
    return;
  }

  // Default path: try the saved "last connected" credentials silently.
  if (trySilentWifiConnect()) {
    return;
  }

  // No credentials available to try. Push the WiFi selection UI (once per
  // session) so the user can connect manually.
  if (!g_promptedForWifiThisSession) {
    promptForWifi();
  }
}

bool StandbyActivity::trySilentWifiConnect() {
  // SD I/O shares SPI with the e-ink panel, so we hold a RenderLock for the
  // duration of the credential file access.
  std::string ssid;
  std::string pass;
  {
    RenderLock lock(*this);
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const std::string last = WIFI_STORE.getLastConnectedSsid();
    if (last.empty()) {
      LOG_DBG("STANDBY", "No lastConnectedSsid for silent connect");
      return false;
    }
    const auto cred = WIFI_STORE.findCredential(last);
    if (!cred) {
      LOG_DBG("STANDBY", "lastConnectedSsid '%s' has no saved credential", last.c_str());
      return false;
    }
    ssid = cred->ssid;
    pass = cred->password;
  }

  WiFi.persistent(false);
  NetworkStartup::setMode(renderer, WIFI_STA);
  // Fresh-slate disconnect (radio off + erase cached AP) so the next begin()
  // latches the exact creds we just loaded, not whatever the radio retained
  // from a previous session. The disconnect(false) form used by the teardown
  // paths is deliberately different — there we want to keep the AP cached
  // for the next silent-connect attempt.
  WiFi.disconnect(true, true);
  delay(100);
  if (pass.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  syncState_ = SyncState::WifiConnecting;
  syncStartMs_ = millis();
  LOG_DBG("STANDBY", "Silent WiFi connect start: %s", ssid.c_str());
  return true;
}

void StandbyActivity::promptForWifi() {
  // Set the flag before pushing so a synchronous edge-case completion can't
  // re-prompt. autoConnect=false: the UI must not retry the same credential
  // we just failed on; let the user pick.
  g_promptedForWifiThisSession = true;
  LOG_DBG("STANDBY", "Prompting WiFi selection UI");
  // ActivityManager owns the picker across frames; stack lifetime is insufficient.
  auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/false);
  if (!activity) {
    LOG_ERR("STANDBY", "OOM: WifiSelectionActivity (%u bytes)", static_cast<unsigned>(sizeof(WifiSelectionActivity)));
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult& result) { onWifiResult(result); });
}

void StandbyActivity::onWifiResult(const ActivityResult& result) {
  if (result.isCancelled) {
    LOG_DBG("STANDBY", "WiFi UI cancelled; clock remains unavailable");
    stopTimeSyncWifi();
    syncState_ = SyncState::Idle;
    return;
  }
  if (TimeUtils::isClockValid()) {
    LOG_DBG("STANDBY", "WiFi UI returned with a trustworthy clock");
    completeTimeSync();
    return;
  }

  LOG_DBG("STANDBY", "WiFi UI returned connected; requesting clock sync");
  beginClockSync();
  requestUpdate();  // refresh header back to "Syncing…"
}

void StandbyActivity::beginClockSync() {
  if (!halClock.requestSync()) {
    LOG_ERR("STANDBY", "Clock sync could not start");
    stopTimeSyncWifi();
    syncState_ = SyncState::Idle;
    return;
  }
  syncState_ = SyncState::ClockSyncing;
  syncStartMs_ = millis();
  LOG_DBG("STANDBY", "Clock sync started");
}

void StandbyActivity::pumpTimeSync() {
  if (syncState_ == SyncState::Idle) return;
  const uint32_t elapsed = millis() - syncStartMs_;

  switch (syncState_) {
    case SyncState::Idle:
      return;
    case SyncState::Delayed:
      if (elapsed < kSyncDelayMs) return;
      syncState_ = SyncState::Idle;
      startTimeSync();
      return;
    case SyncState::WifiConnecting: {
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) {
        beginClockSync();
        return;
      }
      if (status != WL_CONNECT_FAILED && status != WL_NO_SSID_AVAIL && elapsed < kWifiTimeoutMs) return;

      LOG_DBG("STANDBY", "Silent WiFi sync failed (status=%d, t=%ums)", static_cast<int>(status),
              static_cast<unsigned>(elapsed));
      stopTimeSyncWifi();
      syncState_ = SyncState::Idle;
      if (!g_promptedForWifiThisSession)
        promptForWifi();
      else
        requestUpdate();
      return;
    }
    case SyncState::ClockSyncing:
      switch (halClock.syncState()) {
        case ClockSyncState::Succeeded:
          g_promptedForWifiThisSession = false;
          LOG_DBG("STANDBY", "Clock synchronized");
          completeTimeSync();
          return;
        case ClockSyncState::Failed:
          LOG_DBG("STANDBY", "Clock sync failed");
          stopTimeSyncWifi();
          syncState_ = SyncState::Idle;
          return;
        case ClockSyncState::Idle:
          if (elapsed < kNtpTimeoutMs) return;
          LOG_DBG("STANDBY", "Clock sync stopped");
          stopTimeSyncWifi();
          syncState_ = SyncState::Idle;
          return;
        case ClockSyncState::Syncing:
          if (elapsed < kNtpTimeoutMs) return;
          LOG_DBG("STANDBY", "Clock sync timed out");
          stopTimeSyncWifi();
          syncState_ = SyncState::Idle;
          return;
      }
      break;
  }
}

void StandbyActivity::stopTimeSyncWifi() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  // Fully release the WiFi driver so HalPowerManager::setPowerSaving() can
  // actually drop the CPU to LOW_POWER_FREQ — it short-circuits while WiFi is
  // active (see HalPowerManager.cpp). Return value is ignored;
  // ESP_ERR_WIFI_NOT_INIT is fine if we never connected.
  esp_wifi_deinit();
}

void StandbyActivity::completeTimeSync() {
  stopTimeSyncWifi();
  syncState_ = SyncState::Idle;
  lastInputMs_ = millis();
  requestUpdate();
}

void StandbyActivity::processFaceTick(const bool waitForUpdate) {
  if (!currentFace_) return;

  switch (currentFace_->tick()) {
    case StandbyFace::TickResult::None:
      return;
    case StandbyFace::TickResult::Redraw:
      break;
    case StandbyFace::TickResult::RedrawWithGhostCleanup:
#if CROSSPOINT_EMULATED == 0
      if (gpio.isXteinkDevice()) renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
#endif
      break;
  }

  if (waitForUpdate)
    requestUpdateAndWait();
  else
    requestUpdate();
}

bool StandbyActivity::tryLightSleep(const uint32_t idleMs) {
#if CROSSPOINT_EMULATED == 0
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_STANDBY_DIAG)
  static uint32_t lastGateNote = 0;
  if (uint32_t(millis() - lastGateNote) >= 30000) {
    lastGateNote = millis();
    STANDBY_DIAG("gate idle=%lu pic=%u swipe=%u ble=%u rec=%u light=%u sync=%u wifi=%d ready=%u key=%u",
                 static_cast<unsigned long>(idleMs), currentFace_ && currentFace_->needsPicture(),
                 SETTINGS.standbySwipeUnlock, bleinput::isRunning(), HalRecorder::active(), Frontlight.isOn(),
                 syncState_ != SyncState::Idle, static_cast<int>(WiFi.getMode()),
                 powerManager.canStandbyLightSleep(gpio), HalPowerManager::sideKeyBusy());
  }
#endif
  if (mode_ != DisplayMode::Immersive || !currentFace_) return false;
  // Touch does not wake light sleep: stay awake while waiting for a picture to be picked.
  if (currentFace_->needsPicture()) return false;
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  // Any face must have completed its first paint before any bus/rail park.
  if (!drewFrame_) return false;
#endif
#if FREEINK_DEVICE_METALIO_PLUS
  // Side-key/timer candidate only. Preserve first-swipe semantics until touch
  // wake and gesture replay have been independently validated on Plus.
  if (SETTINGS.standbySwipeUnlock || bleinput::isRunning() || HalRecorder::active() || Frontlight.isOn()) return false;
#endif

  const bool syncActive = syncState_ != SyncState::Idle;
  const bool wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
  const bool hardwareAllowed = powerManager.canStandbyLightSleep(gpio);
#ifdef RICKYOS_STANDBY_DIAG
  const bool usbBlocks = false;  // diag: light-sleep with the cable in, to watch the wake
#else
  const bool usbBlocks = gpio.isUsbConnected();
#endif
  if (!standby_time::shouldLightSleep(idleMs, syncActive, wifiActive, usbBlocks, hardwareAllowed)) {
    return false;
  }

#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
  // A static face sleeps until the power key or auto power-off: the board keeps
  // its peripherals parked across timer and shared-IRQ wakes (one restore per
  // standby). millis() keeps counting through light sleep, so the power-off
  // check in loop() fires on the wake at the deadline. A clock or date face
  // sleeps only until its next change (one second past it, so the minute or day
  // has turned), then redraws and parks again.
  const unsigned long powerOffMs = SETTINGS.getStandbyPowerOffMs();
  const unsigned long elapsedMs = millis() - enteredMs_;
  uint32_t seconds =
      powerOffMs == 0 ? 24u * 60u * 60u
                      : static_cast<uint32_t>(std::clamp<unsigned long>(
                            (powerOffMs > elapsedMs ? powerOffMs - elapsedMs : 0) / 1000UL + 1, 1, 24UL * 60UL * 60UL));
  if (currentFace_->wantsClock()) seconds = std::min(seconds, currentFace_->secondsUntilNextWake() + 1u);
#else
  const uint32_t seconds = std::clamp<uint32_t>(currentFace_->secondsUntilNextWake(), 1, kMaxLightSleepSeconds);
#endif
  HalPowerManager::LightSleepWakeReason wakeReason;
  {
    RenderLock lock;
#if FREEINK_DEVICE_METALIO_PLUS
    renderer.waitRefreshComplete();
#endif
    wakeReason = powerManager.lightSleepFor(seconds);
  }
  switch (wakeReason) {
    case HalPowerManager::LightSleepWakeReason::Timer:
      STANDBY_DIAG("wake: timer");
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY)
      // A picture face draws nothing on a timer wake. A clock face redraws, and
      // the refresh lands before the next park.
      if (currentFace_->wantsClock()) {
        processFaceTick(true);
        renderer.waitRefreshComplete();
      }
#else
      processFaceTick(true);
#endif
      return true;
    case HalPowerManager::LightSleepWakeReason::PowerButton:
      // A short press, already over before main.cpp's key poll could see it: leave
      // Standby, as that press does while awake.
      lastInputMs_ = millis();
      STANDBY_DIAG("close: light-sleep wake read as a short press");
      activityManager.closeStandby();
      return true;
    case HalPowerManager::LightSleepWakeReason::PowerButtonHeld:
      // Still down: main.cpp finishes it (release = leave Standby, hold = power off).
      STANDBY_DIAG("wake: key still down");
      lastInputMs_ = millis();
#if !(FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_STATIC_STANDBY))
      requestUpdate();
#endif
      return true;
    case HalPowerManager::LightSleepWakeReason::Failed:
#if FREEINK_DEVICE_METALIO_PLUS
      // A shared IRQ, USB, or failed read is not a key. Bound failed attempts
      // without adding a wake timer or spinning on a persistently asserted IRQ.
      lastInputMs_ = millis() - (standby_time::kLightSleepIdleMs - 5000u);
#else
      // Not the key: the PMU's 30 s battery sample or another expander input. Sleep
      // again after a short pause instead of treating it as input (StandbyTime.h).
      STANDBY_DIAG("wake: other, sleep again in %ums", standby_time::kResleepAfterOtherWakeMs);
      lastInputMs_ = millis() - (standby_time::kLightSleepIdleMs - standby_time::kResleepAfterOtherWakeMs);
#endif
      return true;
  }
#else
  (void)idleMs;
#endif
  return false;
}

void StandbyActivity::loop() {
  if (SETTINGS.standbySwipeUnlock && mappedInput.wasStandbyUnlockGesture()) {
    STANDBY_DIAG("close: swipe unlock");
    finish();
    return;
  }
  // Back includes touch gestures and a header target from the prior page. Neither
  // may bypass the independent unlock switch; physical Back keeps its preference.
  if (!mappedInput.wasBackGesture() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // One shortcut switch owns both entry and exit; the side power key stays independent.
    if (SETTINGS.standbyShortcutEnabled) {
      STANDBY_DIAG("close: Back released");
      finish();
    }
    return;
  }

  // One picture, always full screen: no face switching, paging or inverse. A tap
  // or Confirm only matters while there is no picture yet.
  int tapX = 0;
  int tapY = 0;
  const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
  if (tapped || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lastInputMs_ = millis();
    if (currentFace_ && currentFace_->needsPicture()) {
      const auto action = tapped ? currentFace_->pictureActionAt(tapX, tapY) : StandbyFace::PictureAction::Choose;
      if (action == StandbyFace::PictureAction::Choose) openPicturePicker();
      // The downloader restarts to Home when it is done, like the font downloader.
      if (action == StandbyFace::PictureAction::Download &&
          !startActivityForResultWith<RickyWallpaperDownloadActivity>([this](const ActivityResult&) {
            if (currentFace_) currentFace_->onEnter();
            drewFrame_ = false;
            requestUpdate();
          })) {
        LOG_ERR("STANDBY", "Cannot open the picture downloader");
      }
    }
    return;
  }
  if (mappedInput.wasAnyReleased() || mappedInput.wasSwipe() != MappedInputManager::SwipeDir::None) {
    lastInputMs_ = millis();
  }
  pumpTimeSync();
  // Standby left alone long enough powers off (Settings -> Display & Standby).
  const unsigned long powerOffMs = SETTINGS.getStandbyPowerOffMs();
  if (powerOffMs > 0 && millis() - enteredMs_ >= powerOffMs) {
    activityManager.requestPowerOff();
    return;
  }
  if (tryLightSleep(millis() - lastInputMs_)) return;
  processFaceTick(false);
  return;
}

void StandbyActivity::render(RenderLock&&) {
  if (!currentFace_) return;
  // A static rest image has no status bar. Global notifications must not resend it
  // to the glass; picture changes reset drewFrame_, while clock faces keep ticking.
  if (drewFrame_ && !currentFace_->wantsClock() && !currentFace_->needsPicture()) return;
  {
    int top = 0, right = 0, bottom = 0, left = 0;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const Rect viewport{left, top, renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom};
    if (currentFace_->renderNative(renderer, viewport)) {
      drewFrame_ = true;
      return;
    }
    renderer.clearScreen();
    {
      const GfxRenderer::ClipScope clip(renderer, viewport.x, viewport.y, viewport.width, viewport.height);
      currentFace_->render(renderer, viewport);
    }
    // The rest screen is pure B/W: a clean first frame, then quiet minute updates.
    renderer.displayBuffer(drewFrame_ ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
    drewFrame_ = true;
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  int viewTop = 0;
  int viewRight = 0;
  int viewBottom = 0;
  int viewLeft = 0;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  const Rect faceViewport{viewLeft, viewTop, sw - viewLeft - viewRight, sh - viewTop - viewBottom};
  const int overlayTop = std::max(metrics.topPadding, viewTop);

  renderer.clearScreen();

  // Face renders inside the physically viewable screen regardless of mode. Normal
  // chrome is overlaid without reflow; the unsynced warning remains in Immersive.
  {
    const GfxRenderer::ClipScope clip(renderer, faceViewport.x, faceViewport.y, faceViewport.width,
                                      faceViewport.height);
    currentFace_->render(renderer, faceViewport);
  }

  const bool clockValid = TimeUtils::isClockValid();
  if (!clockValid) {
    UITheme::drawCenteredText(renderer, faceViewport, SMALL_FONT_ID, overlayTop, tr(STR_STANDBY_SYNCING));
  }

  if (mode_ != DisplayMode::Normal) {
    if (inverseMode_) renderer.invertScreen();
    renderer.displayBuffer();
    // Opt-in faces (老黄历) get a 4-level gray LUT enhancement layered on the BW
    // image. Only fires in Immersive — Normal-mode navigation needs the
    // ~300-500ms FAST_REFRESH and can't afford the ~2s gray LUT. inverseMode_
    // short-circuits because invertScreen is BW-only and mixes poorly with gray.
    if (currentFace_->wantsGrayscale() && !inverseMode_) applyGrayscalePass(faceViewport);
    return;
  }

  // Top-center face title (or sync state). Small font, no chrome container,
  // no separator line — Apple Standby-style minimal overlay.
  if (clockValid) {
    UITheme::drawCenteredText(renderer, faceViewport, SMALL_FONT_ID, overlayTop,
                              I18n::getInstance().get(currentFace_->titleId()));
  }

  // Top-right battery icon (no percentage text). Reuses BaseTheme::drawBatteryRight.
  constexpr int kBatW = 16;
  constexpr int kBatH = 12;
  GUI.drawBatteryRight(renderer, Rect{sw - viewRight - kBatW - metrics.contentSidePadding, overlayTop, kBatW, kBatH},
                       /*showPercentage=*/false);

  const uint8_t availFaces = countAvailableFaces(sw, sh);
  const uint8_t curRank = rankOfAvailableFace(sw, sh, faceIndex_);
  drawFaceDots(renderer, sw, sh, availFaces, curRank);

  // Standby stays on FAST_REFRESH end-to-end — no full/half waveform flashes.
  // We accept some long-term ghosting in exchange for a calm, non-blinking face.
  if (inverseMode_) renderer.invertScreen();
  renderer.displayBuffer();
}

// Two extra renders (LSB then MSB) into scratch buffers, composited with the BW
// backup by the gray LUT waveform. Mirrors EpubReaderActivity.cpp:813-837. The
// caller gates invocation (passive Immersive vs interactive on-demand); this
// just runs the pass unconditionally.
void StandbyActivity::applyGrayscalePass(const Rect& viewport) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("STANDBY", "Grayscale pass skipped: storeBwBuffer failed");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  {
    const GfxRenderer::ClipScope clip(renderer, viewport.x, viewport.y, viewport.width, viewport.height);
    currentFace_->render(renderer, viewport);
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  {
    const GfxRenderer::ClipScope clip(renderer, viewport.x, viewport.y, viewport.width, viewport.height);
    currentFace_->render(renderer, viewport);
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

// The same gallery as the 屏保 page: thumbnails, then a preview whose Set action
// installs /sleep.bmp.
void StandbyActivity::openPicturePicker() {
  const bool opened = startActivityForResultWith<StandbyGalleryActivity>(
      [this](const ActivityResult&) {
        if (currentFace_) currentFace_->onEnter();
        drewFrame_ = false;
        lastInputMs_ = millis();
        requestUpdate();
      },
      StandbyPictures::Target::Standby);
  if (!opened) LOG_ERR("STANDBY", "Cannot open the picture gallery");
}
