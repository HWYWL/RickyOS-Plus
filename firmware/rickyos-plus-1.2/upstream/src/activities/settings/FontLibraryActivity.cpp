#include "FontLibraryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "FontInstaller.h"
#include "SdCardFontSystem.h"
#include "TextSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

FontLibraryActivity::FontLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontLibrary", renderer, mappedInput) {}

void FontLibraryActivity::onEnter() {
  RenderLock lock(*this);
  UiListActivity::onEnter();
  closeRouting();
  // Fonts also arrive without the font installer: the general file upload, a computer or
  // USB. Re-scan /fonts and /.fonts on every visit (two small folders) so they show up
  // here without a reboot.
  sdFontSystem.markRegistryDirty();
  sdFontSystem.refreshIfDirty();
  familyIndex_ = -1;
  rebuildRows();
}

void FontLibraryActivity::onExit() {
  Activity::onExit();
  rows_.fill({});
}

const char* FontLibraryActivity::headerTitle() const {
  const auto& families = sdFontSystem.registry().getFamilies();
  return familyIndex_ >= 0 && familyIndex_ < static_cast<int>(families.size()) ? families[familyIndex_].name.c_str()
                                                                               : tr(STR_MANAGE_FONTS);
}

void FontLibraryActivity::rebuildRows() {
  rows_.fill({});
  const auto& families = sdFontSystem.registry().getFamilies();
  if (familyIndex_ >= static_cast<int>(families.size())) familyIndex_ = -1;
  if (familyIndex_ < 0) {
    rows_[0].label = tr(STR_RICKY_FONT_ONLINE);
    rows_[1].label = tr(STR_RICKY_FONT_IMPORT);
    rows_[1].subtitle = tr(STR_RICKY_FONT_IMPORT_TYPES);
    rowCount_ = ROOT_ACTIONS + std::min<int>(families.size(), SdCardFontRegistry::MAX_SD_FAMILIES);
    for (int i = ROOT_ACTIONS; i < rowCount_; ++i) {
      const auto& family = families[i - ROOT_ACTIONS];
      rows_[i].label = family.name.c_str();
      rows_[i].value = family.name == SETTINGS.sdFontFamilyName ? tr(STR_SELECTED) : nullptr;
      rows_[i].subtitle = family.vector ? tr(STR_RICKY_FONT_VECTOR) : tr(STR_RICKY_FONT_BITMAP);
    }
  } else {
    rowCount_ = 2;
    rows_[0].label = tr(STR_RICKY_FONT_USE);
    rows_[1].label = tr(STR_DELETE);
  }
  for (int i = 0; i < rowCount_; ++i) rows_[i].actionValue = static_cast<int16_t>(i);
}

void FontLibraryActivity::buildScreen(UiScreen& screen) {
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  fui::ListProps props;
  props.items = rows_.data();
  props.count = static_cast<uint16_t>(rowCount_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  props.rowGap = std::max<int16_t>(6, screen.theme().listRowGap);
  syncListViewport(screen, props);
  screen.list(props);
}

void FontLibraryActivity::refreshAfterChild() {
  RenderLock lock(*this);
  closeRouting();
  sdFontSystem.refreshIfDirty();
  rebuildRows();
  requestUpdate();
}

void FontLibraryActivity::showInfo(const StrId message) {
  static constexpr StrId ok[] = {StrId::STR_OK_BUTTON};
  popup_.show(message, ok, 1, 0, [](int) {});
  requestUpdate();
}

void FontLibraryActivity::openTextSettings(const bool useSelectedFamily) {
  const auto& families = sdFontSystem.registry().getFamilies();
  if (useSelectedFamily && (familyIndex_ < 0 || familyIndex_ >= static_cast<int>(families.size()))) return;
  if (useSelectedFamily && families[familyIndex_].name.size() >= sizeof(SETTINGS.sdFontFamilyName)) {
    showInfo(StrId::STR_RICKY_FONT_NAME_TOO_LONG);
    return;
  }
  auto child = makeUniqueNoThrow<TextSettingsActivity>(
      renderer, mappedInput, &sdFontSystem.registry(),
      useSelectedFamily ? TextSettingsActivity::Tab::Size : TextSettingsActivity::Tab::Family,
      useSelectedFamily ? TextSettingsActivity::InitialFontState::Changed
                        : TextSettingsActivity::InitialFontState::Unchanged);
  if (!child) {
    LOG_ERR("FONTLIB", "OOM: TextSettingsActivity");
    showInfo(StrId::STR_MEMORY_ERROR);
    return;
  }
  if (useSelectedFamily) {
    RenderLock lock(*this);
    snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", families[familyIndex_].name.c_str());
    SETTINGS.sdFontFlashPreload = 0;
    SETTINGS.saveToFile();
  }
  closeRouting();
  startActivityForResult(std::move(child), [this](const ActivityResult&) { refreshAfterChild(); });
}

void FontLibraryActivity::confirmDelete() {
  const auto& families = sdFontSystem.registry().getFamilies();
  if (familyIndex_ < 0 || familyIndex_ >= static_cast<int>(families.size())) return;
  const auto& family = families[familyIndex_];
  if (!FontInstaller::isValidFamilyName(family.name.c_str()) ||
      !SdCardFontRegistry::findFamilyRoot(family.name.c_str())) {
    // Loose files / unsupported directory names are not safe deleteFamily targets.
    showInfo(StrId::STR_RICKY_FONT_DELETE_ON_SD);
    return;
  }
  const std::string name = family.name;  // One bounded name, only for the confirmation lifetime.
  const bool started = startActivityForResultWith<ConfirmationActivity>(
      [this, name](const ActivityResult& result) {
        if (result.isCancelled) {
          refreshAfterChild();
          return;
        }
        bool removed = false;
        {
          RenderLock lock(*this);
          closeRouting();
          sdFontSystem.releaseLoadedFont(renderer);
          FontInstaller installer(sdFontSystem.registry());
          removed = installer.deleteFamily(name.c_str()) == FontInstaller::Error::OK;
          sdFontSystem.markRegistryDirty();
          familyIndex_ = -1;
          nav = rootNav_;
        }
        refreshAfterChild();
        if (!removed) showInfo(StrId::STR_FAILED_LOWER);
      },
      tr(STR_DELETE), name);
  if (!started) showInfo(StrId::STR_MEMORY_ERROR);
}

void FontLibraryActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount_) return;
  app.clearTapFlash();
  if (familyIndex_ >= 0) {
    if (index == 0)
      openTextSettings(true);
    else
      confirmDelete();
  } else if (index == 0) {
    if (!startActivityForResultWith<FontDownloadActivity>([this](const ActivityResult&) { refreshAfterChild(); })) {
      showInfo(StrId::STR_MEMORY_ERROR);
    }
  } else if (index == 1) {
    showInfo(StrId::STR_RICKY_FONT_IMPORT_GUIDE);
  } else {
    RenderLock lock(*this);
    closeRouting();
    rootNav_ = nav;
    familyIndex_ = index - ROOT_ACTIONS;
    nav.reset();
    rebuildRows();
    requestUpdate();
  }
}

void FontLibraryActivity::onBackButton() {
  if (familyIndex_ < 0) {
    finish();
    return;
  }
  RenderLock lock(*this);
  closeRouting();
  familyIndex_ = -1;
  nav = rootNav_;
  rebuildRows();
  requestUpdate();
}

bool FontLibraryActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void FontLibraryActivity::render(RenderLock&& lock) {
  if (popup_.isActive()) renderPopupUnderlay();
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
