#include "UsbDriveActivity.h"

#if FREEINK_CAP_USB_MSC || defined(RICKYOS_USB_PREVIEW)

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>

#include "BleInput.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "activities/home/RickyStorageActivity.h"
#include <LibraryBuilder.h>
#include <LibraryIncoming.h>
#include "util/BookCoverLoader.h"

namespace fui = freeink::ui;

void UsbDriveActivity::onEnter() {
#if !defined(RICKYOS_USB_PREVIEW)
  bleinput::stop();
#endif
  Activity::onEnter();
  BookCoverLoader::forgetDrawnCovers();  // the computer may change books and their caches
  resetUi();
  app.setScreen(&UsbDriveActivity::driveScreen, this);

#if defined(RICKYOS_USB_PREVIEW)
  // The browser preview must never take ownership of the host's SD storage.
  preparing = false;
  state = State::WaitingForHost;
  requestUpdate();
#else

  // Show the safety instructions before giving the raw SD card to the USB host.
  requestUpdateAndWait();
  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = State::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = State::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
#endif
}

void UsbDriveActivity::onExit() {
#if !defined(RICKYOS_USB_PREVIEW)
  if (!restartRequested) Storage.endUsbDrive();
  RickyStorageActivity::invalidateScan();  // files may have arrived or left
  if (!startFailed) {
    // The computer may have added or removed books anywhere; the shelf refreshes
    // its enrolled sources on its next entry (no walk here, USB is still unwinding).
    library::LibraryIncoming::markUnknownChanges();
    library::markLibraryIndexDirty();
  }
#endif
  Activity::onExit();
}

void UsbDriveActivity::loop() {
#if defined(RICKYOS_USB_PREVIEW)
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture()) {
    activityManager.goToInxRecent();
  }
#else
  if (!startFailed) {
    const State nextState = Storage.usbDriveState();
    if (nextState != state) {
      LOG_INF("USB", "state %d -> %d", static_cast<int>(state), static_cast<int>(nextState));
      state = nextState;
      requestUpdate();
    }
  }
#if RICKYOS_USB_DIAG
  // Diagnostics build: the serial port is the USB disk now, so show the sector traffic
  // on the panel. A transfer that never finishes shows as a growing "now" figure.
  if (millis() - diagShownAt_ >= 3000) {
    diagShownAt_ = millis();
    const UsbDriveStats s = Storage.usbDriveStats();
    char text[sizeof(diagText_)];
    // Idle time in whole 10 s steps, so a quiet bus does not repaint every 3 s.
    snprintf(text, sizeof(text), "W %lu  %lu.%lu MB  R %lu\nretry %lu  fail %lu  max %lu ms  now %lu ms\nidle %lu s  host %lu s  ss %lu/%u%s",
             static_cast<unsigned long>(s.writes), static_cast<unsigned long>(s.sectorsWritten / 2048),
             static_cast<unsigned long>(s.sectorsWritten % 2048 * 10 / 2048), static_cast<unsigned long>(s.reads),
             static_cast<unsigned long>(s.retries), static_cast<unsigned long>(s.failures),
             static_cast<unsigned long>(s.maxOpMs), static_cast<unsigned long>(s.inFlightMs),
             static_cast<unsigned long>(s.idleMs / 10000 * 10), static_cast<unsigned long>(s.hostQuietMs / 10000 * 10),
             static_cast<unsigned long>(s.startStops), static_cast<unsigned>(s.lastStartStop),
             s.suspended ? "  suspended" : "");
    if (s.startStops != diagStartStops_) {
      diagStartStops_ = s.startStops;
      LOG_INF("USB", "start/stop #%lu code %u", static_cast<unsigned long>(s.startStops),
              static_cast<unsigned>(s.lastStartStop));
    }
    if (strcmp(text, diagText_) != 0) {
      if (s.suspended != diagSuspended_) LOG_INF("USB", "host %s", s.suspended ? "suspended" : "resumed");
      diagSuspended_ = s.suspended;
      memcpy(diagText_, text, sizeof(diagText_));
      requestUpdate();
    }
  }
#endif

  // USB suspend is a normal host power-saving state, not an eject or cable
  // removal. Keep the SD card owned by MSC until the host actually ejects or
  // disconnects, otherwise macOS can lose a mounted volume while idle.

  if (state == State::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  if (!startFailed && state == State::IoError) {
    if (!forcedDisconnectRequested) {
      forcedDisconnectRequested = true;
      forcedDisconnectRequestedAt = millis();
      LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB Drive host disconnect");
      }
    } else if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect timed out; forcing restart");
      restartToHome();
    }
    return;
  }

  const bool canExitWithInput = state == State::WaitingForHost || startFailed;
  if (canExitWithInput && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture())) {
    restartToHome();
    return;
  }
  // macOS drops a disk without ejecting it when a power transition times out behind a
  // long read burst (Finder previewing a folder of large books over this slow link),
  // and then sends nothing at all, so no state change ever comes. Back always leaves.
  if (state == State::Connected && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    LOG_INF("USB", "left by Back while connected");
    restartToHome();
    return;
  }

  if (state == State::Ejected || state == State::Disconnected || state == State::Unsupported) {
    restartToHome();
  }
#endif
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
#if defined(RICKYOS_USB_PREVIEW)
                 tr(STR_RICKY_USB_PREVIEW));
#else
                 tr(STR_USB_DRIVE));
#endif

  renderUi();

  if (state == State::WaitingForHost || state == State::Connected || startFailed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void UsbDriveActivity::driveScreen(UiScreen& screen, void* user) {
  static_cast<UsbDriveActivity*>(user)->buildDriveScreen(screen);
}

void UsbDriveActivity::buildDriveScreen(UiScreen& screen) const {
  const char* message = nullptr;
  const char* detail = nullptr;
  const char* secondaryDetail = nullptr;
#if defined(RICKYOS_USB_PREVIEW)
  message = tr(STR_RICKY_USB_PREVIEW);
  detail = tr(STR_RICKY_USB_PREVIEW_DESC);
  secondaryDetail = tr(STR_RICKY_USB_PREVIEW_STEPS);
#else
  if (preparing) {
    message = tr(STR_USB_DRIVE_PREPARING);
    detail = tr(STR_USB_DRIVE_EJECT_HINT);
  } else {
    switch (state) {
      case State::WaitingForHost:
        message = tr(STR_USB_DRIVE_WAITING);
        break;
      case State::Connected:
        message = tr(STR_USB_DRIVE_CONNECTED);
        detail = tr(STR_USB_DRIVE_CONNECT_DELAY);
        secondaryDetail = tr(STR_USB_DRIVE_EJECT_HINT);
#if RICKYOS_USB_DIAG
        secondaryDetail = diagText_;
#endif
        break;
      case State::IoError:
        message = startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR);
        break;
      case State::Ejected:
      case State::Disconnected:
      case State::Unsupported:
        return;
    }
  }
#endif

  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});

  auto messageStyle = SETTINGS.uiTheme == CrossPointSettings::INX ? screen.theme().titleText : screen.theme().smallText;
  messageStyle.align = fui::TextAlign::Center;
  messageStyle.bold = true;
  messageStyle.maxLines = 2;
  auto detailStyle = SETTINGS.uiTheme == CrossPointSettings::INX ? screen.theme().bodyText : screen.theme().smallText;
  detailStyle.align = fui::TextAlign::Center;
  detailStyle.maxLines = 3;

  const fui::Rect body = screen.body();
  const int16_t messageHeight = fui::measureWrappedText(screen.target(), message, messageStyle, body.width).height;
  const int16_t detailHeight =
      detail ? fui::measureWrappedText(screen.target(), detail, detailStyle, body.width).height : 0;
  const int16_t secondaryDetailHeight =
      secondaryDetail ? fui::measureWrappedText(screen.target(), secondaryDetail, detailStyle, body.width).height : 0;
  const int16_t gap = detail ? screen.theme().spaceMd : 0;
  const int16_t secondaryGap = secondaryDetail ? screen.theme().spaceMd : 0;
  const int16_t totalHeight =
      static_cast<int16_t>(messageHeight + gap + detailHeight + secondaryGap + secondaryDetailHeight);
  if (body.height > totalHeight) screen.spacer(static_cast<int16_t>((body.height - totalHeight) / 2));

  screen.target().text(screen.takeTop(messageHeight, gap), message, messageStyle);
  if (detail) {
    screen.target().text(screen.takeTop(detailHeight, secondaryGap), detail, detailStyle);
  }
  if (secondaryDetail) {
    screen.target().text(screen.takeTop(secondaryDetailHeight), secondaryDetail, detailStyle);
  }
}

void UsbDriveActivity::restartToHome() {
#if defined(RICKYOS_USB_PREVIEW)
  activityManager.goToInxRecent();
#else
  if (restartRequested) return;
  restartRequested = true;
  const unsigned long started = millis();
  Storage.endUsbDrive();
  LOG_INF("USB", "drive released in %lu ms", millis() - started);
  delay(20);
  restartToHomeAfterStorageHandoff();
#endif
}

#endif  // FREEINK_CAP_USB_MSC || RICKYOS_USB_PREVIEW
