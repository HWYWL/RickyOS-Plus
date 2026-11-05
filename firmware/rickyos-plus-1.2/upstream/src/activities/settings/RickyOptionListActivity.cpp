#include "RickyOptionListActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "components/RickyPageUi.h"
#include "components/UITheme.h"

namespace {
namespace fui = freeink::ui;

constexpr StrId kGestureLabels[] = {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                                    StrId::STR_INVERTED_TAP, StrId::STR_DISABLED};
static_assert(std::size(kGestureLabels) == CrossPointSettings::PAGE_TURN_GESTURE_COUNT);
constexpr StrId kDirectionLabels[] = {StrId::STR_PAGE_TURN_LTR, StrId::STR_PAGE_TURN_RTL};
constexpr StrId kMenuLabels[] = {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE_UP};
static_assert(std::size(kMenuLabels) == CrossPointSettings::SHOW_READER_MENU_COUNT);
constexpr StrId kSideKeyLabels[] = {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED,
                                    StrId::STR_NEXT_NEXT, StrId::STR_PREV_PREV};
constexpr StrId kLongPressLabels[] = {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                                      StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION};

template <size_t N>
constexpr uint8_t countOf(const StrId (&)[N]) {
  return static_cast<uint8_t>(N);
}
}  // namespace

namespace RickyOptionLists {
const RickyOptionList& gestures() {
  static const RickyOptionRow rows[] = {
      {StrId::STR_RICKY_GESTURE_BACK, &SETTINGS.rickyGestureBack, nullptr, 0, StrId::STR_RICKY_GESTURE_GLOBAL},
      {StrId::STR_RICKY_GESTURE_HOME, &SETTINGS.rickyGestureHome},
      {StrId::STR_RICKY_STANDBY_SWIPE_UNLOCK, &SETTINGS.standbySwipeUnlock},
      {StrId::STR_RICKY_GESTURE_CONTROL, &SETTINGS.rickyGestureControl},
      {StrId::STR_RICKY_GESTURE_STATUS, &SETTINGS.rickyGestureStatus},
      {StrId::STR_RICKY_GESTURE_TOUCH_TURN, &SETTINGS.touchReaderControls, nullptr, 0,
       StrId::STR_RICKY_GESTURE_READING},
      {StrId::STR_NEXT_PAGE_GESTURE, &SETTINGS.pageTurnGesture, kGestureLabels, countOf(kGestureLabels)},
      {StrId::STR_PREV_PAGE_GESTURE, &SETTINGS.previousPageGesture, kGestureLabels, countOf(kGestureLabels)},
      {StrId::STR_PAGE_TURN_DIRECTION, &SETTINGS.pageTurnDirection, kDirectionLabels, countOf(kDirectionLabels)},
      {StrId::STR_SHOW_READER_MENU, &SETTINGS.showReaderMenu, kMenuLabels, countOf(kMenuLabels)},
  };
  static const RickyOptionList list{StrId::STR_RICKY_GESTURES, rows, static_cast<int>(std::size(rows))};
  return list;
}

const RickyOptionList& keys() {
  // The strip under the screen is left / middle / right; the middle key is Back.
  static const RickyOptionRow rows[] = {
#if FREEINK_DEVICE_METALIO_PLUS
      // The Plus sets each key's short/long press on its own page; the Read Pico rows
      // (left/right page keys, long press, middle-key standby) duplicated those there.
      {StrId::STR_PLUS_TOUCH_FEEDBACK, &SETTINGS.plusTouchFeedback},
#else
      {StrId::STR_RICKY_KEYS_SIDE, &SETTINGS.sideButtonLayout, kSideKeyLabels, countOf(kSideKeyLabels)},
      {StrId::STR_RICKY_KEYS_LONG_PRESS, &SETTINGS.longPressButtonBehavior, kLongPressLabels,
       countOf(kLongPressLabels)},
      {StrId::STR_RICKY_KEYS_STANDBY, &SETTINGS.standbyShortcutEnabled},
#endif
  };
  static const RickyOptionList list{StrId::STR_RICKY_KEYS, rows, static_cast<int>(std::size(rows))};
#if !((defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4) || defined(SIMULATOR_DEVICE_EEGO_A4)) && \
    !FREEINK_DEVICE_METALIO_PLUS
  // A Home key device has no Back-to-Standby shortcut on Home: drop its row. The Plus
  // list above never had it (its one row is touch feedback, which must stay).
  static const RickyOptionList homeKeyList{StrId::STR_RICKY_KEYS, rows + 1, static_cast<int>(std::size(rows)) - 1};
  if (BoardConfig::hasHomeKey()) return homeKeyList;
#endif
  return list;
}
}  // namespace RickyOptionLists

const char* RickyOptionListActivity::headerTitle() const { return I18N.get(list.title); }

bool RickyOptionListActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void RickyOptionListActivity::render(RenderLock&& lock) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void RickyOptionListActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  fui::ListProps props;
  props.rowProvider = &RickyOptionListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(std::max(0, list.count));
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void RickyOptionListActivity::provideRow(void* ctx, uint16_t index, fui::ListItem& item) {
  const auto* self = static_cast<const RickyOptionListActivity*>(ctx);
  if (index >= self->list.count) return;
  const auto& row = self->list.rows[index];
  item.label = I18N.get(row.label);
  item.actionValue = static_cast<int16_t>(index);
  if (row.section != StrId::STR_NONE_OPT) item.sectionHeading = I18N.get(row.section);
  if (row.options && row.optionCount) {
    item.value = I18N.get(row.options[*row.field % row.optionCount]);
  } else {
    GUI.setCheckboxRow(item, *row.field != 0);
  }
}

void RickyOptionListActivity::activateIndex(const int index) {
  if (index < 0 || index >= list.count || optionPopup.isActive()) return;
  app.clearTapFlash();
  nav.selected = index;
  const RickyOptionRow& item = list.rows[index];
  if (!item.options) {
    *item.field = *item.field ? 0 : 1;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
  uint8_t* field = item.field;
  optionPopup.show(item.label, item.options, item.optionCount, *field % item.optionCount, [field](const int chosen) {
    *field = static_cast<uint8_t>(chosen);
    SETTINGS.saveToFile();
  });
  requestUpdate();
}
