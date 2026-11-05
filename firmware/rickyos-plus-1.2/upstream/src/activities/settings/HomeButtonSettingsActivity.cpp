#include "HomeButtonSettingsActivity.h"

#include <utility>

#include "components/UITheme.h"
#if FREEINK_DEVICE_METALIO_PLUS
#include "RickyOptionListActivity.h"
#endif

namespace fui = freeink::ui;

#if FREEINK_DEVICE_METALIO_PLUS
namespace {
constexpr StrId RESET_ACTIONS[] = {StrId::STR_CANCEL, StrId::STR_CONFIRM};
constexpr StrId KEY_LABELS[] = {StrId::STR_HOME_BUTTON, StrId::STR_PLUS_KEY_AI, StrId::STR_PLUS_KEY_PREVIOUS, StrId::STR_PLUS_KEY_NEXT,
                                StrId::STR_PLUS_KEY_UP, StrId::STR_PLUS_KEY_DOWN};
constexpr StrId KEY_ACTIONS[] = {StrId::STR_PLUS_KEY_DEFAULT, StrId::STR_IGNORE, StrId::STR_BACK, StrId::STR_CONFIRM, StrId::STR_PREV_PAGE,
                                StrId::STR_NEXT_PAGE, StrId::STR_HOME_SHORTCUT, StrId::STR_READER_MENU, StrId::STR_FORCE_REFRESH,
                                StrId::STR_TOGGLE_FRONTLIGHT, StrId::STR_RICKY_CC_TITLE};
static_assert(sizeof(KEY_ACTIONS)/sizeof(KEY_ACTIONS[0]) == plus_keys::Count);
}
#endif

const char* HomeButtonSettingsActivity::headerTitle() const {
#if FREEINK_DEVICE_METALIO_PLUS
  return I18N.get(keyPage < 0 ? StrId::STR_RICKY_KEYS : KEY_LABELS[keyPage]);
#else
  return I18N.get(StrId::STR_HOME_BUTTON);
#endif
}

#if FREEINK_DEVICE_METALIO_PLUS
void HomeButtonSettingsActivity::onBackButton() {
  if (keyPage < 0) { finish(); return; }
  keyPage = -1;
  nav.reset();
  app.clearTapFlash();
  requestUpdate();
}
#endif

bool HomeButtonSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void HomeButtonSettingsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount() || optionPopup.isActive()) return;
  mappedInput.resetHomeButtonInput();
  app.clearTapFlash();
  nav.selected = index;
#if FREEINK_DEVICE_METALIO_PLUS
  if (keyPage < 0) {
    if (index == listCount() - 1) {
      optionPopup.show(StrId::STR_PLUS_KEY_RESET, RESET_ACTIONS, 2, 0, [this](int selected) {
        if (selected != 1) return;
        for (unsigned i = 0; i < plus_keys::FIELD_COUNT; ++i)
          SETTINGS.plusKeyActions[i] = i % 2 ? plus_keys::Ignore : plus_keys::Default;
        SETTINGS.homeButtonTapAction = static_cast<uint8_t>(HomeButtonAction::Home);
        SETTINGS.homeButtonDoubleTapAction = static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight);
        SETTINGS.homeButtonLongPressAction = static_cast<uint8_t>(HomeButtonAction::ReaderMenu);
        SETTINGS.sideButtonLayout = CrossPointSettings::PREV_NEXT;
        SETTINGS.longPressButtonBehavior = CrossPointSettings::OFF;
        SETTINGS.standbyShortcutEnabled = 0;
        SETTINGS.plusTouchFeedback = 1;
        SETTINGS.saveToFile();
        mappedInput.resetHomeButtonInput();
      });
    } else if (index >= 6) {
      const auto& row = RickyOptionLists::keys().rows[index - 6];
      if (row.options) {
        optionPopup.show(row.label, row.options, row.optionCount, *row.field, [field = row.field](int selected) {
          *field = static_cast<uint8_t>(selected);
          SETTINGS.saveToFile();
        });
      } else {
        *row.field = !*row.field;
        if (row.field == &SETTINGS.plusTouchFeedback && !*row.field) mappedInput.stopHapticFeedback();
        SETTINGS.saveToFile();
      }
    } else { keyPage = index; nav.reset(); }
    requestUpdate();
    return;
  }
  if (keyPage > 0) {
    const unsigned field = (keyPage - 1) * 2 + index;
    optionPopup.show(index ? StrId::STR_HOME_BUTTON_LONG_PRESS : StrId::STR_HOME_BUTTON_TAP, KEY_ACTIONS,
                     plus_keys::Count, SETTINGS.plusKeyActions[field], [field](int selected) {
                       SETTINGS.plusKeyActions[field] = static_cast<uint8_t>(selected);
                       SETTINGS.saveToFile();
                     });
    requestUpdate();
    return;
  }
#endif
  const auto field = home_button::FIELDS[index];
  const uint8_t value = SETTINGS.*field;
  const int current = value < static_cast<uint8_t>(HomeButtonAction::Count) ? value : 0;
  optionPopup.show(home_button::GESTURE_LABELS[index], home_button::ACTION_LABELS,
                   static_cast<int>(HomeButtonAction::Count), current, [field](int selected) {
                     SETTINGS.*field = static_cast<uint8_t>(selected);
                     SETTINGS.saveToFile();
                   });
  requestUpdate();
}

void HomeButtonSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void HomeButtonSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  for (int i = 0; i < listCount(); ++i) {
#if FREEINK_DEVICE_METALIO_PLUS
    if (keyPage < 0) {
      rows[i] = {};
      rows[i].actionValue = i;
      if (i < 6) rows[i].label = I18N.get(KEY_LABELS[i]);
      else if (i == listCount() - 1) rows[i].label = I18N.get(StrId::STR_PLUS_KEY_RESET);
      else {
        const auto& row = RickyOptionLists::keys().rows[i - 6];
        rows[i].label = I18N.get(row.label);
        if (row.options) rows[i].value = I18N.get(row.options[*row.field < row.optionCount ? *row.field : 0]);
        else rows[i].value = I18N.get(*row.field ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
      }
      continue;
    }
    if (keyPage > 0) {
      rows[i].label = I18N.get(i ? StrId::STR_HOME_BUTTON_LONG_PRESS : StrId::STR_HOME_BUTTON_TAP);
      rows[i].value = I18N.get(KEY_ACTIONS[plus_keys::valid(SETTINGS.plusKeyActions[(keyPage-1)*2+i], i)]);
      rows[i].actionValue = i;
      continue;
    }
#endif
    rows[i].label = I18N.get(home_button::GESTURE_LABELS[i]);
    rows[i].actionValue = static_cast<int16_t>(i);
    const uint8_t value = SETTINGS.*home_button::FIELDS[i];
    rows[i].value = value < static_cast<uint8_t>(HomeButtonAction::Count) ? I18N.get(home_button::ACTION_LABELS[value])
                                                                          : I18N.get(home_button::ACTION_LABELS[0]);
  }
  fui::ListProps props;
  props.items = rows;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  // Keep the gesture name and its current action at the same visual weight.
  props.labelText = screen.theme().smallText;
  // A default smallText style is treated as inherited by screen.list().
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
