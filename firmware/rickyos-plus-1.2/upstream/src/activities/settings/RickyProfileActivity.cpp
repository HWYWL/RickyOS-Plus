#include "RickyProfileActivity.h"
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "RickyAvatarPickerActivity.h"
#include "RickyHomePhraseActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyPageUi.h"
#include "components/RickyProfile.h"
#include "components/UITheme.h"

void RickyProfileActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(ACTION_THEME, &RickyProfileActivity::onThemeEvent, this);
}

void RickyProfileActivity::onThemeEvent(const freeink::ui::ActionEvent& event, void* user) {
  auto* self = static_cast<RickyProfileActivity*>(user);
  self->nav.selected = 4;
  self->setCompanion(event.value == 1 ? CrossPointSettings::RICKY_COMPANION_CAT
                                      : CrossPointSettings::RICKY_COMPANION_DOG);
}

void RickyProfileActivity::setCompanion(const uint8_t companion) {
  app.clearTapFlash();
  failed = false;
  if (companion == SETTINGS.rickyCompanion) return;
  RenderLock lock(*this);
  const uint8_t previous = SETTINGS.rickyCompanion;
  SETTINGS.rickyCompanion = companion;
  failed = !SETTINGS.saveToFile();
  if (failed) SETTINGS.rickyCompanion = previous;
  requestUpdate();
}

const char* RickyProfileActivity::headerTitle() const { return tr(STR_RICKY_PROFILE); }

void RickyProfileActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(
      fui::Insets{static_cast<int16_t>(theme.spaceLg * 2), theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);
  const int bodyHeight = target.lineHeight(theme.bodyText.font);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  const auto black = fui::Paint::solid(fui::Color::Black);
  constexpr uint8_t radius = 6;
  const int selected = RickyPageUi::syncNav(nav, listCount());
  const bool focus = showMainTabContentSelection();
  auto small = theme.smallText;
  small.maxLines = 1;
  auto centered = small;
  centered.align = fui::TextAlign::Center;

  // Avatar first, centered, with its own action underneath.
  const int avatarSize = std::min<int>(160, screen.body().width / 3);
  const auto avatarBlock = screen.takeTop(avatarSize + gap + smallHeight, gap * 4);
  const Rect avatar{avatarBlock.x + (avatarBlock.width - avatarSize) / 2, avatarBlock.y, avatarSize, avatarSize};
  RickyProfile::drawAvatar(renderer, avatar);
  if (focus && selected == 1)
    target.stroke(RickyPageUi::uiRect(Rect{avatar.x - 6, avatar.y - 6, avatar.width + 12, avatar.height + 12}), black,
                  3, static_cast<uint8_t>(avatarSize / 2 + 6));
  target.text(fui::Rect{avatarBlock.x, static_cast<int16_t>(avatar.y + avatarSize + gap), avatarBlock.width,
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_CHANGE_AVATAR), centered);
  screen.frame().hit(avatarBlock, ACTION_ROW, 1, fui::InputTouch);

  // Nickname reads like a field; tapping it or the primary button edits it.
  target.text(screen.takeTop(smallHeight, gap / 2), tr(STR_RICKY_NICKNAME), small);
  const int controlHeight = bodyHeight + gap * 2;
  const auto field = screen.takeTop(controlHeight, failed ? gap / 2 : gap * 2);
  target.stroke(field, black, focus && selected == 0 ? 3 : 1, radius);
  auto value = theme.bodyText;
  value.maxLines = 1;
  target.text(fui::Rect{static_cast<int16_t>(field.x + gap), static_cast<int16_t>(field.y + gap),
                        static_cast<int16_t>(field.width - gap * 2), static_cast<int16_t>(bodyHeight)},
              RickyProfile::nickname(), value);
  screen.frame().hit(field, ACTION_ROW, 0, fui::InputTouch);
  if (failed) {
    auto error = small;
    error.maxLines = 2;
    target.text(screen.takeTop(smallHeight, gap * 2), tr(STR_RICKY_PROFILE_FAILED), error);
  }

  // The home-page note belongs to the person too: same field style, opens its editor.
  target.text(screen.takeTop(smallHeight, gap / 2), tr(STR_RICKY_PHRASE_TITLE), small);
  const auto phrase = screen.takeTop(controlHeight, gap * 3);
  target.stroke(phrase, black, focus && selected == 3 ? 3 : 1, radius);
  target.text(fui::Rect{static_cast<int16_t>(phrase.x + gap), static_cast<int16_t>(phrase.y + gap),
                        static_cast<int16_t>(phrase.width - gap * 2), static_cast<int16_t>(bodyHeight)},
              RickyProfile::homePhrase(), value);
  screen.frame().hit(phrase, ACTION_ROW, 3, fui::InputTouch);

  // Theme: two halves, the chosen companion filled.
  target.text(screen.takeTop(smallHeight, gap / 2), tr(STR_RICKY_THEME), small);
  const auto theme2 = screen.takeTop(controlHeight, gap * 3);
  target.stroke(theme2, black, focus && selected == 4 ? 3 : 1, radius);
  const bool cat = SETTINGS.rickyCat();
  const int16_t half = static_cast<int16_t>(theme2.width / 2);
  const fui::Rect halves[2] = {{theme2.x, theme2.y, half, theme2.height},
                               {static_cast<int16_t>(theme2.x + half), theme2.y,
                                static_cast<int16_t>(theme2.width - half), theme2.height}};
  const char* labels[2] = {tr(STR_RICKY_THEME_DOG), tr(STR_RICKY_THEME_CAT)};
  for (int i = 0; i < 2; ++i) {
    const bool chosen = (i == 1) == cat;
    if (chosen) target.fill(halves[i], black, radius);
    auto label = theme.bodyText;
    label.align = fui::TextAlign::Center;
    label.maxLines = 1;
    label.color = chosen ? fui::Color::White : fui::Color::Black;
    target.text(fui::Rect{halves[i].x, static_cast<int16_t>(halves[i].y + gap), halves[i].width,
                          static_cast<int16_t>(bodyHeight)},
                labels[i], label);
    screen.frame().hit(halves[i], ACTION_THEME, i, fui::InputTouch);
  }

  const auto primary = screen.takeTop(controlHeight, gap);
  target.fill(primary, black, radius);
  auto inverse = theme.bodyText;
  inverse.align = fui::TextAlign::Center;
  inverse.maxLines = 1;
  inverse.color = fui::Color::White;
  target.text(
      fui::Rect{primary.x, static_cast<int16_t>(primary.y + gap), primary.width, static_cast<int16_t>(bodyHeight)},
      tr(STR_RICKY_EDIT_NICKNAME), inverse);
  screen.frame().hit(primary, ACTION_ROW, 0, fui::InputTouch);

  const auto secondary = screen.takeTop(controlHeight, gap);
  target.stroke(secondary, black, focus && selected == 2 ? 3 : 1, radius);
  auto outline = inverse;
  outline.color = fui::Color::Black;
  target.text(fui::Rect{secondary.x, static_cast<int16_t>(secondary.y + gap), secondary.width,
                        static_cast<int16_t>(bodyHeight)},
              tr(STR_RICKY_RESET_AVATAR), outline);
  screen.frame().hit(secondary, ACTION_ROW, 2, fui::InputTouch);
}

bool RickyProfileActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void RickyProfileActivity::activateIndex(int index) {
  app.clearTapFlash();
  failed = false;
  if (index == 0) {
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          if (result.isCancelled) return;
          const auto* entry = std::get_if<KeyboardResult>(&result.data);
          if (!entry) return;
          RenderLock lock(*this);
          char previous[sizeof(SETTINGS.rickyNickname)];
          memcpy(previous, SETTINGS.rickyNickname, sizeof(previous));
          const int length = utf8SafeTruncateBuffer(
              entry->text.c_str(), std::min<int>(entry->text.size(), sizeof(SETTINGS.rickyNickname) - 1));
          memcpy(SETTINGS.rickyNickname, entry->text.data(), length);
          SETTINGS.rickyNickname[length] = '\0';
          failed = !SETTINGS.saveToFile();
          if (failed) memcpy(SETTINGS.rickyNickname, previous, sizeof(previous));
        },
        tr(STR_RICKY_NICKNAME), std::string(SETTINGS.rickyNickname), sizeof(SETTINGS.rickyNickname) - 1);
  } else if (index == 1) {
    // Built-in portraits first; importing from the SD card lives on that page.
    startActivityForResultWith<RickyAvatarPickerActivity>([this](const ActivityResult&) {
      waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
      requestUpdate();
    });
  } else if (index == 3) {
    startActivityForResultWith<RickyHomePhraseActivity>([this](const ActivityResult&) { requestUpdate(); });
  } else if (index == 4) {
    // Confirm on the row toggles; a touch on either half picks that side.
    setCompanion(SETTINGS.rickyCat() ? CrossPointSettings::RICKY_COMPANION_DOG
                                     : CrossPointSettings::RICKY_COMPANION_CAT);
  } else if (index == 2) {
    RenderLock lock(*this);
    failed = !RickyProfile::setAvatar("");
    requestUpdate();
  }
}
