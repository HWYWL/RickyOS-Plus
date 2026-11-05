#include "RickyHelpActivity.h"

#include <I18n.h>

#include "activities/reader/QrDisplayActivity.h"
#include "components/RickyDesignTokens.h"
#include "network/RickyOtaPolicy.h"

namespace {
constexpr StrId titles[] = {StrId::STR_RICKY_HELP_START, StrId::STR_RICKY_HELP_DISPLAY_TITLE,
                            StrId::STR_RICKY_HELP_UPDATE_TITLE, StrId::STR_RICKY_HELP_FEEDBACK_TITLE,
                            StrId::STR_RICKY_HELP_WEBSITE};
constexpr StrId paragraphs[][3] = {
    {StrId::STR_RICKY_HELP_START_1, StrId::STR_RICKY_HELP_START_2, StrId::STR_RICKY_HELP_START_3},
    {StrId::STR_RICKY_HELP_DISPLAY_1, StrId::STR_RICKY_HELP_DISPLAY_2, StrId::STR_RICKY_HELP_DISPLAY_3},
    {StrId::STR_RICKY_HELP_UPDATE_1, StrId::STR_RICKY_HELP_UPDATE_2, StrId::STR_RICKY_HELP_UPDATE_3},
    {StrId::STR_RICKY_HELP_FEEDBACK_1, StrId::STR_RICKY_HELP_FEEDBACK_2, StrId::STR_RICKY_HELP_FEEDBACK_3},
};
}  // namespace

const char* RickyHelpActivity::headerTitle() const {
  return detail_ < 0 ? tr(STR_RICKY_HELP_FEEDBACK) : I18N.get(titles[detail_]);
}

void RickyHelpActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen({static_cast<int16_t>(bounds.y), 0,
                                     static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  for (int i = 0; i < listCount(); ++i) {
    rows_[i] = {};
    rows_[i].label = I18N.get(detail_ < 0 ? titles[i] : paragraphs[detail_][i]);
    rows_[i].actionValue = i;
  }
  freeink::ui::ListProps props;
  props.items = rows_;
  props.count = listCount();
  props.action = detail_ < 0 ? ACTION_ROW : freeink::ui::NO_ACTION;
  props.inputMask = freeink::ui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = detail_ < 0 ? 2 : 6;
  props.rowGap = ricky::ds::space::k3;
  syncListViewport(screen, props);
  if (detail_ >= 0) props.selectedIndex = -1;
  screen.list(props);
}

void RickyHelpActivity::activateIndex(int index) {
  if (detail_ >= 0 || index < 0 || index >= 5) return;
  app.clearTapFlash();
  if (index == 4) {
    startActivityForResultWith<QrDisplayActivity>([](const ActivityResult&) {}, ricky_ota::SITE_BASE);
    return;
  }
  RenderLock lock(*this);
  detail_ = index;
  nav.reset();
  requestUpdate();
}

void RickyHelpActivity::onBackButton() {
  if (detail_ < 0) {
    UiListActivity::onBackButton();
    return;
  }
  RenderLock lock(*this);
  detail_ = -1;
  nav.reset();
  requestUpdate();
}
