#include "RecorderActivity.h"
#if FREEINK_DEVICE_METALIO_PLUS
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
namespace fui = freeink::ui;
void RecorderActivity::onEnter() {
  UiListActivity::onEnter();
  waitRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  reload();
  shownPreparing = HalRecorder::preparing();
  shownState = HalRecorder::state();
  shownSeconds = HalRecorder::seconds();
}
void RecorderActivity::onExit() {
  HalRecorder::stop();
  Activity::onExit();
}
void RecorderActivity::prepareForSleep() {
  // ActivityManager already holds RenderLock while invoking sleep callbacks.
  HalRecorder::stop();
}
void RecorderActivity::reload() {
  count = HalRecorder::list(offset, files.data(), files.size(), more);
  if (!count && offset) {
    offset = 0;
    count = HalRecorder::list(0, files.data(), files.size(), more);
  }
  for (size_t i = 0; i < count; ++i)
    snprintf(durations[i].data(), durations[i].size(), "%02u:%02u", unsigned(files[i].seconds / 60),
             unsigned(files[i].seconds % 60));
  nav.reset();
  requestUpdate();
}
int RecorderActivity::listCount() const {
  if (HalRecorder::active()) return 1;
  return 1 + count + (offset ? 1 : 0) + (more ? 1 : 0);
}
const char* RecorderActivity::statusLabel() const {
  switch (HalRecorder::state()) {
    case HalRecorder::State::Recording:
      return (HalRecorder::preparing() ? tr(STR_REC_PREPARING) : tr(STR_REC_RECORDING));
    case HalRecorder::State::Playing:
      return tr(STR_REC_PLAYING);
    case HalRecorder::State::Saved:
      return tr(STR_REC_SAVED);
    case HalRecorder::State::Error:
      return tr(STR_REC_ERROR);
    default:
      return (count ? tr(STR_REC_READY) : tr(STR_REC_EMPTY));
  }
}
void RecorderActivity::drawChrome() {
  const auto& m = UITheme::getInstance().getMetrics();
  const uint32_t seconds = HalRecorder::seconds();
  if (HalRecorder::preparing())
    snprintf(status, sizeof(status), "%s", statusLabel());
  else
    snprintf(status, sizeof(status), "%s  %02u:%02u", statusLabel(), unsigned(seconds / 60), unsigned(seconds % 60));
  GUI.drawHeader(renderer, Rect{0, m.topPadding, renderer.getScreenWidth(), m.headerHeight}, tr(STR_REC_TITLE), status);
}
void RecorderActivity::buildScreen(UiScreen& screen) {
  const auto& m = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(m.topPadding + m.headerHeight), 0,
                                      static_cast<int16_t>(m.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(m.verticalSpacing));
  rows = {};
  const bool active = HalRecorder::active();
  const bool preparing = HalRecorder::preparing();
  rows[0].label = preparing ? tr(STR_REC_CANCEL_PREPARING) : (active ? tr(STR_REC_STOP) : tr(STR_REC_START));
  rows[0].subtitle = preparing ? tr(STR_REC_WAIT_HINT) : tr(STR_REC_FORMAT);
  if (!active) {
    for (size_t i = 0; i < count; ++i) {
      rows[i + 1].label = files[i].name;
      rows[i + 1].subtitle = tr(STR_REC_TAP_PLAY);
      rows[i + 1].value = durations[i].data();
    }
    size_t n = count + 1;
    if (offset) rows[n++].label = tr(STR_REC_PREVIOUS);
    if (more) rows[n].label = tr(STR_REC_NEXT);
  }
  for (int i = 0; i < listCount(); ++i) rows[i].actionValue = i;
  fui::ListProps p;
  p.items = rows.data();
  p.count = listCount();
  p.action = ACTION_ROW;
  p.inputMask = fui::InputTouch;
  syncListViewport(screen, p);
  screen.list(p);
}
bool RecorderActivity::handleCustomInput() {
  if (waitRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitRelease = false;
    return true;
  }
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return true;
  const auto before = HalRecorder::state();
  HalRecorder::poll();
  const auto now = HalRecorder::state();
  const uint32_t seconds = HalRecorder::seconds();
  const bool preparing = HalRecorder::preparing();
  if (shownPreparing && !preparing && now == HalRecorder::State::Recording && SETTINGS.plusTouchFeedback)
    mappedInput.pulseHapticFeedback();
  if (now != before || now != shownState || preparing != shownPreparing) {
    shownPreparing = preparing;
    if (!HalRecorder::active())
      reload();
    else
      nav.reset();
    shownState = now;
    shownSeconds = seconds;
    requestUpdate();
  } else if (HalRecorder::active() && seconds / 5 != shownSeconds / 5) {
    shownSeconds = seconds;
    requestUpdate();  // bounded e-ink updates, no animation
  }
  return false;
}
void RecorderActivity::activateIndex(int index) {
  RenderLock lock(*this);
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (HalRecorder::active()) {
    HalRecorder::stop();
    reload();
  } else if (!index)
    HalRecorder::startRecording();
  else if (index <= static_cast<int>(count))
    HalRecorder::play(files[index - 1].name, SETTINGS.speakerVolume);
  else if (offset && index == static_cast<int>(count) + 1) {
    offset -= pageSize;
    reload();
  } else if (more) {
    offset += pageSize;
    reload();
  }
  nav.reset();
  shownPreparing = HalRecorder::preparing();
  shownState = HalRecorder::state();
  shownSeconds = HalRecorder::seconds();
  requestUpdate();
}
void RecorderActivity::onBackButton() {
  if (HalRecorder::active()) {
    RenderLock lock(*this);
    HalRecorder::stop();
    reload();
    shownPreparing = HalRecorder::preparing();
    shownState = HalRecorder::state();
    shownSeconds = HalRecorder::seconds();
  } else
    activityManager.goToApps();
}
#endif
