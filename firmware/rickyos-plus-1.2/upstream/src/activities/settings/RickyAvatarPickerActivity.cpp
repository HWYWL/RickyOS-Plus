#include "RickyAvatarPickerActivity.h"
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "activities/home/FileBrowserActivity.h"
#include "components/RickyPageUi.h"
#include "components/RickyProfile.h"
#include "components/UITheme.h"
#include "util/RickyStorageLayout.h"

const char* RickyAvatarPickerActivity::headerTitle() const { return tr(STR_RICKY_CHANGE_AVATAR); }

void RickyAvatarPickerActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(fui::Insets{static_cast<int16_t>(theme.spaceLg), theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);
  const int pad = gap + gap / 2;
  const int bodyHeight = target.lineHeight(theme.bodyText.font);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  const auto black = fui::Paint::solid(fui::Color::Black);
  const int selected = RickyPageUi::syncNav(nav, listCount());
  const bool focus = showMainTabContentSelection();
  const int current = RickyProfile::presetAvatarIndex();

  if (failed) {
    auto error = theme.smallText;
    error.maxLines = 2;
    target.text(screen.takeBottom(smallHeight * 2, gap), tr(STR_RICKY_PROFILE_FAILED), error);
  }
  // Three columns of round portraits, at most the art's native size.
  constexpr int columns = 3;
  const int rows = (RickyAvatars::COUNT + columns - 1) / columns;
  const int importHeight = bodyHeight + pad * 2;
  const int cellWidth = (screen.body().width - gap * (columns - 1)) / columns;
  const int cellHeight =
      std::min<int>(cellWidth, (screen.body().height - importHeight - gap * 2 - gap * (rows - 1)) / rows);
  const auto body = screen.takeTop(cellHeight * rows + gap * (rows - 1), gap * 2);
  const int avatar = std::min<int>(RickyAvatars::SIZE, cellHeight - gap * 2);
  for (int i = 0; i < RickyAvatars::COUNT; ++i) {
    const fui::Rect cell{static_cast<int16_t>(body.x + i % columns * (cellWidth + gap)),
                         static_cast<int16_t>(body.y + i / columns * (cellHeight + gap)),
                         static_cast<int16_t>(cellWidth), static_cast<int16_t>(cellHeight)};
    const Rect disc{cell.x + (cell.width - avatar) / 2, cell.y + (cell.height - avatar) / 2, avatar, avatar};
    RickyProfile::drawPresetAvatar(renderer, disc, i);
    // The chosen portrait wears a second, outer ring; key focus frames the cell.
    if (current == i)
      target.stroke(RickyPageUi::uiRect(Rect{disc.x - 7, disc.y - 7, disc.width + 14, disc.height + 14}), black, 3,
                    static_cast<uint8_t>(avatar / 2 + 7));
    if (focus && selected == i) target.stroke(cell, black, 2, RickyPageUi::CARD_RADIUS);
    screen.frame().hit(cell, ACTION_ROW, i, fui::InputTouch);
  }
  // Importing stays available right under the built-in choices.
  const auto import = screen.takeTop(importHeight, 0);
  RickyPageUi::card(target, import, focus && selected == IMPORT);
  auto label = theme.bodyText;
  label.maxLines = 1;
  target.text(fui::Rect{static_cast<int16_t>(import.x + pad), static_cast<int16_t>(import.y + pad),
                        static_cast<int16_t>(import.width - pad * 2 - 24), static_cast<int16_t>(bodyHeight)},
              tr(STR_RICKY_AVATAR_TYPES), label);
  RickyPageUi::chevron(target, fui::Rect{static_cast<int16_t>(import.right() - 24 - pad), import.y, 24, import.height});
  screen.frame().hit(import, ACTION_ROW, IMPORT, fui::InputTouch);
}

bool RickyAvatarPickerActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void RickyAvatarPickerActivity::activateIndex(const int index) {
  app.clearTapFlash();
  failed = false;
  if (index == IMPORT) {
    startActivityForResultWith<FileBrowserActivity>(
        [this](const ActivityResult& result) {
          waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          if (result.isCancelled) return;
          const auto* entry = std::get_if<FilePathResult>(&result.data);
          if (!entry) return;
          RenderLock lock(*this);
          closeRouting();
          GUI.drawPopup(renderer, tr(STR_RICKY_AVATAR_IMPORTING));
          failed = !RickyProfile::importAvatar(entry->path);
          if (!failed) finish();
        },
        RickyStorageLayout::IMAGES, FileBrowserActivity::Mode::PickAvatar);
    return;
  }
  char value[16] = "";
  snprintf(value, sizeof(value), "@avatar:%d", index);
  {
    RenderLock lock(*this);
    failed = !RickyProfile::setAvatar(value);
  }
  if (failed)
    requestUpdate();
  else
    finish();
}
