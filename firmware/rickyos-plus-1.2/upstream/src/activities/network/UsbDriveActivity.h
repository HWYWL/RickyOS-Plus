#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_USB_MSC || defined(RICKYOS_USB_PREVIEW)

#include <HalStorage.h>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

class UsbDriveActivity final : public Activity, private UiAppHost {
 public:
  UsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbDrive", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Connected || (!startFailed && state == State::IoError); }
  bool requiresExclusiveStorageLoop() const override {
#if defined(RICKYOS_USB_PREVIEW)
    return false;
#else
    return true;
#endif
  }

 private:
  using State = UsbDriveState;

  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  static constexpr unsigned long START_FAILURE_TIMEOUT_MS = 30UL * 1000UL;
  static constexpr unsigned long FORCED_DISCONNECT_TIMEOUT_MS = 1000UL;

  static void driveScreen(UiScreen& screen, void* user);
  void buildDriveScreen(UiScreen& screen) const;
  void restartToHome();

  State state = State::Unsupported;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool forcedDisconnectRequested = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long forcedDisconnectRequestedAt = 0;
#if RICKYOS_USB_DIAG
  char diagText_[140] = "";
  bool diagSuspended_ = false;
  uint32_t diagStartStops_ = 0;
  unsigned long diagShownAt_ = 0;
#endif
};

#endif  // FREEINK_CAP_USB_MSC || RICKYOS_USB_PREVIEW
