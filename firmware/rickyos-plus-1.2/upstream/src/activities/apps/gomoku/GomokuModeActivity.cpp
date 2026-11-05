#include "GomokuModeActivity.h"

#include <I18n.h>

#include "GomokuMenuActivity.h"
#include "SkillGomokuActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

GomokuModeActivity::GomokuModeActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("GomokuMode", renderer, input) {}

void GomokuModeActivity::onEnter() {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  UiListActivity::onEnter();
  rows[0].label = tr(STR_SGM_TRADITIONAL);
  rows[0].subtitle = tr(STR_SGM_TRADITIONAL_DESC);
  rows[0].actionValue = 0;
  rows[1].label = tr(STR_SGM_TITLE);
  rows[1].subtitle = tr(STR_SGM_DESC);
  rows[1].actionValue = 1;
  releaseBarrier = true;
}

const char* GomokuModeActivity::headerTitle() const { return tr(STR_GOMOKU_TITLE); }

bool GomokuModeActivity::handleCustomInput() {
  if (!releaseBarrier) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      !mappedInput.isPressed(MappedInputManager::Button::Back))
    releaseBarrier = false;
  return true;  // consume the inherited release as well
}

void GomokuModeActivity::activateIndex(int index) {
  app.clearTapFlash();
  if (index == 0) activityManager.replaceActivityWith<GomokuMenuActivity>();
  if (index == 1) activityManager.replaceActivityWith<SkillGomokuActivity>();
}

void GomokuModeActivity::onBackButton() { activityManager.goToApps(); }

void GomokuModeActivity::buildScreen(UiScreen& screen) {
  const auto& m = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(safe.y + m.topPadding + m.headerHeight),
                                                static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                                static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                                static_cast<int16_t>(safe.x)});
  screen.spacer(m.verticalSpacing);
  fui::ListProps props;
  props.items = rows;
  props.count = 2;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}
