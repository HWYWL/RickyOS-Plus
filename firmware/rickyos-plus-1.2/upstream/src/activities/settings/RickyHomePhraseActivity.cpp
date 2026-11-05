#include "RickyHomePhraseActivity.h"
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyProfile.h"

const char* RickyHomePhraseActivity::headerTitle() const { return tr(STR_RICKY_PHRASE_TITLE); }

void RickyHomePhraseActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(bounds.y), static_cast<int16_t>(renderer.getScreenWidth() - bounds.x - bounds.width),
      static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), static_cast<int16_t>(bounds.x)});
  const auto& theme = screen.theme();
  screen.insetContent(fui::Insets{theme.spaceSm, theme.spaceLg, theme.spaceSm, theme.spaceLg});
  auto preview = theme.bodyText;
  preview.maxLines = 2;
  screen.target().text(screen.takeTop(screen.target().lineHeight(preview.font) * 2, theme.spaceLg),
                       RickyProfile::homePhrase(), preview);
  auto hint = theme.smallText;
  hint.maxLines = 2;
  screen.target().text(screen.takeTop(screen.target().lineHeight(hint.font) * 2, theme.spaceLg),
                       tr(STR_RICKY_PHRASE_HINT), hint);
  if (failed)
    screen.target().text(screen.takeTop(screen.target().lineHeight(hint.font), theme.spaceSm),
                         tr(STR_RICKY_PROFILE_FAILED), hint);
  rows[0].label = tr(STR_RICKY_PHRASE_EDIT);
  rows[1].label = tr(STR_RICKY_PHRASE_RESET);
  for (int i = 0; i < listCount(); ++i) rows[i].actionValue = i;
  fui::ListProps props;
  props.items = rows.data();
  props.count = rows.size();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowGap = std::max<int16_t>(6, theme.listRowGap);
  syncListViewport(screen, props);
  screen.list(props);
}

bool RickyHomePhraseActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void RickyHomePhraseActivity::activateIndex(int index) {
  app.clearTapFlash();
  if (index == 0) {
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          if (result.isCancelled) return;
          const auto* entry = std::get_if<KeyboardResult>(&result.data);
          if (!entry) return;
          RenderLock lock(*this);
          failed = !RickyProfile::setHomePhrase(entry->text);
          requestUpdate();
        },
        tr(STR_RICKY_PHRASE_TITLE), std::string(RickyProfile::homePhrase()), sizeof(SETTINGS.rickyHomePhrase) - 1);
  } else if (index == 1) {
    RenderLock lock(*this);
    failed = !RickyProfile::setHomePhrase("");
    requestUpdate();
  }
}
