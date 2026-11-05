#include "LibraryCategoriesActivity.h"

#include <I18n.h>

#include "activities/util/KeyboardEntryActivity.h"

namespace fui = freeink::ui;
const char* LibraryCategoriesActivity::headerTitle() const {
  return count ? tr(STR_LIBRARY_ASSIGN_CATEGORY) : tr(STR_LIBRARY_CATEGORIES);
}
void LibraryCategoriesActivity::onEnter() {
  RenderLock lock(*this);
  UiListActivity::onEnter();
  waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                   mappedInput.isPressed(MappedInputManager::Button::Back);
  failed = categories.load() == library::LibraryCategories::Load::Error;
  if (!failed && count) failed = !categories.selectionMasks(hashes.get(), count, any, all);
  requestUpdate();
}
void LibraryCategoriesActivity::onExit() {
  hashes.reset();
  UiListActivity::onExit();
}
int LibraryCategoriesActivity::listCount() const {
  if (failed) return 1;
  int total = leadingRows();
  for (int i = 0; i < library::LibraryCategories::MAX_CATEGORIES; ++i)
    if (*categories.name(i)) ++total;
  return total;
}
int LibraryCategoriesActivity::slotForRow(int row) const {
  row -= leadingRows();
  if (row < 0) return -1;
  for (int i = 0; i < library::LibraryCategories::MAX_CATEGORIES; ++i)
    if (*categories.name(i) && row-- == 0) return i;
  return -1;
}
void LibraryCategoriesActivity::provideRow(void* ctx, uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<LibraryCategoriesActivity*>(ctx);
  item.actionValue = index;
  if (self.failed) {
    item.label = tr(STR_LIBRARY_CATEGORY_ERROR);
    return;
  }
  if (index < self.leadingRows()) {
    item.label = self.count ? (index == 0 ? tr(STR_SAVE) : tr(STR_LIBRARY_NEW_CATEGORY_ASSIGN))
                            : (index == 0   ? tr(STR_LIBRARY_ALL_CATEGORIES)
                               : index == 1 ? tr(STR_LIBRARY_UNCATEGORIZED)
                                            : tr(STR_LIBRARY_NEW_CATEGORY));
    return;
  }
  const int slot = self.slotForRow(index);
  if (slot < 0) return;
  item.label = self.categories.name(slot);
  if (self.count) {
    const uint16_t bit = uint16_t(1u << slot);
    const bool checked = (self.touched & bit) ? (self.enabled & bit) : (self.all & bit);
    item.value = checked                                       ? tr(STR_LIBRARY_CATEGORY_SELECTED)
                 : (!(self.touched & bit) && (self.any & bit)) ? tr(STR_LIBRARY_CATEGORY_MIXED)
                                                               : "";
  }
}
void LibraryCategoriesActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  fui::ListProps props;
  props.rowProvider = &provideRow;
  props.rowProviderCtx = this;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}
void LibraryCategoriesActivity::showError() {
  const char* choices[] = {tr(STR_DONE)};
  options.showMessage(tr(STR_LIBRARY_CATEGORIES), tr(STR_LIBRARY_CATEGORY_ERROR), choices, 1, 0, [](int) {});
  requestUpdate();
}
void LibraryCategoriesActivity::editName(int slot) {
  app.clearTapFlash();
  startActivityForResultWith<KeyboardEntryActivity>(
      [this, slot](const ActivityResult& result) {
        waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                         mappedInput.isPressed(MappedInputManager::Button::Back);
        if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;
        const auto& name = std::get<KeyboardResult>(result.data).text;
        RenderLock lock(*this);
        if (slot >= 0) {
          if (!categories.rename(slot, name.c_str())) showError();
          requestUpdate();
          return;
        }
        const int created = categories.create(name.c_str());
        if (created < 0) {
          showError();
          requestUpdate();
          return;
        }
        if (count) {
          // Made while assigning: the selected books go straight in, with whatever
          // else was ticked, and the assignment is saved.
          const uint16_t bit = uint16_t(1u << created);
          touched |= bit;
          enabled |= bit;
          if (!categories.assign(hashes.get(), count, touched, enabled)) {
            showError();
            requestUpdate();
            return;
          }
          setResult(ActivityResult{IntervalResult{0}});
          app.clearTapFlash();
          finish();
          return;
        }
        requestUpdate();
      },
      slot < 0 ? tr(STR_LIBRARY_NEW_CATEGORY) : tr(STR_RENAME), categories.name(slot), size_t(48), InputType::Text);
}
void LibraryCategoriesActivity::activateIndex(int row) {
  if (failed) {
    onBackButton();
    return;
  }
  if (count) {
    if (row == 0) {
      if (!categories.assign(hashes.get(), count, touched, enabled)) {
        showError();
        return;
      }
      setResult(ActivityResult{IntervalResult{0}});
      app.clearTapFlash();
      finish();
    } else if (row == 1)
      editName();
    else {
      const int slot = slotForRow(row);
      if (slot < 0) return;
      RenderLock lock(*this);
      const uint16_t bit = uint16_t(1u << slot);
      const bool wasSet = (touched & bit) ? (enabled & bit) : (all & bit);
      touched |= bit;
      if (wasSet)
        enabled &= ~bit;
      else
        enabled |= bit;
      app.clearTapFlash();
      requestUpdate();
    }
  } else if (row < 2 || slotForRow(row) >= 0) {
    setResult(ActivityResult{IntervalResult{row < 2 ? row : slotForRow(row) + 2}});
    app.clearTapFlash();
    finish();
  } else if (row == 2)
    editName();
}
void LibraryCategoriesActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  app.clearTapFlash();
  finish();
}
void LibraryCategoriesActivity::onRowLongPress(int row) {
  const int slot = slotForRow(row);
  if (failed || slot < 0) return;
  const char* choices[] = {tr(STR_RENAME), tr(STR_DELETE)};
  options.show(tr(STR_LIBRARY_CATEGORIES), categories.name(slot), choices, 2, 0, [this, slot](int choice) {
    if (choice == 0) {
      editName(slot);
      return;
    }
    const char* confirm[] = {tr(STR_CANCEL), tr(STR_DELETE)};
    options.showMessage(categories.name(slot), tr(STR_LIBRARY_DELETE_CATEGORY_NOTE), confirm, 2, 0,
                        [this, slot](int action) {
                          if (action != 1) return;
                          RenderLock lock(*this);
                          if (!categories.erase(slot))
                            showError();
                          else {
                            const uint16_t bit = uint16_t(1u << slot);
                            touched &= ~bit;
                            enabled &= ~bit;
                            any &= ~bit;
                            all &= ~bit;
                          }
                          nav.reset();
                          requestUpdate();
                        });
  });
  requestUpdate();
}
bool LibraryCategoriesActivity::handleCustomInput() {
  if (waitForButtons) {
    waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                     mappedInput.isPressed(MappedInputManager::Button::Back);
    return true;
  }
  if (options.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 1000)) {
    onRowLongPress(activeNav().selected);
    return true;
  }
  return false;
}
void LibraryCategoriesActivity::render(RenderLock&& lock) {
  if (options.isActive()) renderPopupUnderlay();
  if (options.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
