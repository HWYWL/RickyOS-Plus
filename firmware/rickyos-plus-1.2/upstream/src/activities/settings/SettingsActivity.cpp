#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#if (FREEINK_DEVICE_MURPHY_M4 || FREEINK_CAP_HAPTIC) && !defined(SIMULATOR)
#include <HalGPIO.h>
#endif
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "AboutActivity.h"
#include "AppVisibilitySettingsActivity.h"
#include "BluetoothSettingsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#include "DictionaryDownloadActivity.h"
#include "FontDownloadActivity.h"
#include "FontLibraryActivity.h"
#include "HomeButtonSettingsActivity.h"
#include "InxItemLayout.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "ReadingStatsSettingsActivity.h"
#include "RickyHelpActivity.h"
#include "RickyProfileActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/RickyOptionListActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/ImageViewerActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/RickyDesignUi.h"
#include "components/RickyPageUi.h"
#include "components/RickyProfile.h"
#include "components/RickyRowList.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/rickyPageIcons.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/SystemSettingsReset.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId OK_OPTION[] = {StrId::STR_OK_BUTTON};
constexpr uint64_t BYTES_PER_TENTH_GB = 100000000ULL;

enum class AboutRow : uint8_t {
  FirmwareName,
  FirmwareVersion,
  License,
  DeviceModel,
  ChipModel,
  ChipTemperature,
  WifiMacAddress,
  Uptime,
  HeapFreeTotal,
  LargestHeapBlock,
  PsramHeapFreeTotal,
  SdUsedTotal,
  Count,
};

enum class StorageLoadState : uint8_t { Loading, Available, Unavailable };

class InxAboutActivity final : public Activity {
 public:
  InxAboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("About", renderer, mappedInput) {}

  void onEnter() override {
    Activity::onEnter();
    heapInfo = HalSystem::getHeapInfo();
    deviceName = BoardConfig::ACTIVE.name;
#ifdef SIMULATOR
    chipModel = "Simulator";
    wifiMacAvailable = HalSystem::getDeviceId(wifiMac);
    uptimeSeconds = millis() / 1000;
#else
    chipModel = HalSystem::getChipModel();
    wifiMacAvailable = HalSystem::getWifiStationMac(wifiMac);
    float temperature = 0.0f;
    temperatureAvailable = HalSystem::getChipTemperatureCelsius(temperature);
    if (temperatureAvailable) chipTemperatureCelsius = static_cast<int>(std::lround(temperature));
    uptimeSeconds = HalSystem::getUptimeSeconds();
#endif
    storageLoadState = StorageLoadState::Loading;
    requestUpdateAndWait();
    storageLoadState =
        Storage.getSpace(sdTotalBytes, sdFreeBytes) ? StorageLoadState::Available : StorageLoadState::Unavailable;
    requestUpdate();
  }

  void loop() override {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int pageItems = GUI.getListPageItems(contentRect().height, false);
    if (pageItems <= 0 || rowCount() <= pageItems) return;
    int nextPageStart = pageStart;
    const auto swipe = mappedInput.wasSwipe();
    if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) || swipe == MappedInputManager::SwipeDir::Up) {
      nextPageStart = ButtonNavigator::nextPageIndex(pageStart, rowCount(), pageItems);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
               swipe == MappedInputManager::SwipeDir::Down) {
      nextPageStart = ButtonNavigator::previousPageIndex(pageStart, rowCount(), pageItems);
    }
    if (nextPageStart == pageStart) return;
    pageStart = nextPageStart;
    requestUpdate();
  }

  void render(RenderLock&&) override {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    GUI.drawHeader(renderer, Rect{safeArea.x, safeArea.y + metrics.topPadding, safeArea.width, metrics.headerHeight},
                   tr(STR_ABOUT));
    const Rect content = contentRect();
    GUI.drawList(
        renderer, content, rowCount(), pageStart,
        [this](const int index) {
          static constexpr StrId LABELS[] = {
              StrId::STR_ABOUT_FIRMWARE_NAME,
              StrId::STR_ABOUT_FIRMWARE_VERSION,
              StrId::STR_ABOUT_LICENSE,
              StrId::STR_ABOUT_DEVICE_MODEL,
              StrId::STR_ABOUT_CHIP_MODEL,
              StrId::STR_ABOUT_CHIP_TEMPERATURE,
              StrId::STR_ABOUT_WIFI_MAC_ADDRESS,
              StrId::STR_ABOUT_UPTIME,
              StrId::STR_ABOUT_HEAP_FREE_TOTAL,
              StrId::STR_ABOUT_LARGEST_HEAP_BLOCK,
              StrId::STR_ABOUT_PSRAM_HEAP_FREE_TOTAL,
              StrId::STR_ABOUT_SD_USED_TOTAL,
          };
          static_assert(sizeof(LABELS) / sizeof(*LABELS) == static_cast<size_t>(AboutRow::Count));
          return std::string(I18N.get(LABELS[static_cast<size_t>(rowAt(index))]));
        },
        nullptr, nullptr, [this](const int index) { return rowValue(rowAt(index)); }, false, nullptr, false);
    const bool canPage = rowCount() > GUI.getListPageItems(content.height, false);
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), "", canPage ? tr(STR_DIR_UP) : "", canPage ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
  }

 private:
  Rect contentRect() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    return Rect{safeArea.x, safeArea.y + metrics.topPadding + metrics.headerHeight, safeArea.width,
                safeArea.height - metrics.topPadding - metrics.headerHeight};
  }

  bool hasPsram() const {
#ifdef SIMULATOR
    return false;
#else
    return heapInfo.totalPsramBytes > 0;
#endif
  }

  int rowCount() const {
    constexpr int psramRows = 1;
    return static_cast<int>(AboutRow::Count) - (hasPsram() ? 0 : psramRows);
  }

  AboutRow rowAt(const int index) const {
    if (!hasPsram() && index >= static_cast<int>(AboutRow::PsramHeapFreeTotal)) {
      return static_cast<AboutRow>(index + 1);
    }
    return static_cast<AboutRow>(index);
  }

  std::string rowValue(const AboutRow row) const {
    char value[48];
    switch (row) {
      case AboutRow::FirmwareName:
        return tr(STR_CROSSPOINT);
      case AboutRow::FirmwareVersion:
        return CROSSPOINT_VERSION;
      case AboutRow::DeviceModel:
        return deviceName ? deviceName : tr(STR_NOT_AVAILABLE);
      case AboutRow::ChipModel:
        return chipModel ? chipModel : tr(STR_NOT_AVAILABLE);
      case AboutRow::WifiMacAddress:
        if (!wifiMacAvailable) return tr(STR_NOT_AVAILABLE);
        snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X", wifiMac[0], wifiMac[1], wifiMac[2], wifiMac[3],
                 wifiMac[4], wifiMac[5]);
        return value;
      case AboutRow::ChipTemperature:
        if (!temperatureAvailable) return tr(STR_NOT_AVAILABLE);
        snprintf(value, sizeof(value), "%d", chipTemperatureCelsius);
        return value;
      case AboutRow::Uptime: {
        const uint64_t totalMinutes = uptimeSeconds / 60;
        snprintf(value, sizeof(value), "%llu:%02llu:%02llu", static_cast<unsigned long long>(totalMinutes / (24 * 60)),
                 static_cast<unsigned long long>(totalMinutes / 60 % 24),
                 static_cast<unsigned long long>(totalMinutes % 60));
        return value;
      }
      case AboutRow::HeapFreeTotal:
        snprintf(value, sizeof(value), "%lu / %lu", static_cast<unsigned long>(heapInfo.freeBytes / 1024),
                 static_cast<unsigned long>(heapInfo.totalBytes / 1024));
        return value;
      case AboutRow::LargestHeapBlock:
        snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(heapInfo.largestFreeBlockBytes / 1024));
        return value;
      case AboutRow::PsramHeapFreeTotal:
#ifdef SIMULATOR
        return tr(STR_NOT_AVAILABLE);
#else
        snprintf(value, sizeof(value), "%lu / %lu", static_cast<unsigned long>(heapInfo.freePsramBytes / 1024),
                 static_cast<unsigned long>(heapInfo.totalPsramBytes / 1024));
        return value;
#endif
      case AboutRow::License:
        // MIT: the notice travels with the firmware (full text: LICENSE, and the install site).
        return "MIT";
      case AboutRow::SdUsedTotal: {
        switch (storageLoadState) {
          case StorageLoadState::Loading:
            return tr(STR_LOADING);
          case StorageLoadState::Unavailable:
            return tr(STR_NOT_AVAILABLE);
          case StorageLoadState::Available:
            break;
        }
        const uint64_t usedTenths = (sdTotalBytes - sdFreeBytes + BYTES_PER_TENTH_GB / 2) / BYTES_PER_TENTH_GB;
        const uint64_t totalTenths = (sdTotalBytes + BYTES_PER_TENTH_GB / 2) / BYTES_PER_TENTH_GB;
        snprintf(value, sizeof(value), "%llu.%llu / %llu.%llu", static_cast<unsigned long long>(usedTenths / 10),
                 static_cast<unsigned long long>(usedTenths % 10), static_cast<unsigned long long>(totalTenths / 10),
                 static_cast<unsigned long long>(totalTenths % 10));
        return value;
      }
      case AboutRow::Count:
        return {};
    }
    return {};
  }

  HalSystem::HeapInfo heapInfo{};
  HalSystem::DeviceId wifiMac{};
  const char* deviceName = nullptr;
  const char* chipModel = nullptr;
  uint64_t uptimeSeconds = 0;
  uint64_t sdTotalBytes = 0;
  uint64_t sdFreeBytes = 0;
  int chipTemperatureCelsius = 0;
  bool wifiMacAvailable = false;
  bool temperatureAvailable = false;
  StorageLoadState storageLoadState = StorageLoadState::Loading;
  int pageStart = 0;
};
}  // namespace

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Settings", renderer, mappedInput) {}

void SettingsActivity::reorganizeRickySettings() {
  // These bounded vectors live only with this Activity and are released with
  // the other lists before a memory-hungry child. Moves retain all enum values,
  // persistence keys, accessors and action handlers without cloning settings.
  librarySettings.reserve(6);
  connectionSettings.reserve(8);
  fontSettings.reserve(1);  // One moved action, not another copy of font catalog/enum data.
  const auto moveMatching = [](std::vector<SettingInfo>& source, std::vector<SettingInfo>& destination,
                               const auto& matches) {
    for (auto it = source.begin(); it != source.end();) {
      if (matches(*it)) {
        destination.push_back(std::move(*it));
        it = source.erase(it);
      } else {
        ++it;
      }
    }
  };
  const auto removeMatching = [](std::vector<SettingInfo>& list, const auto& matches) {
    list.erase(std::remove_if(list.begin(), list.end(), matches), list.end());
  };

  // Rows that live on their own pages or do nothing on this device leave the lists:
  // - the Standby & power-off page has what Standby and power-off show, the picture's
  //   fit and filter, when Standby starts and when it powers off;
  // - every touch gesture is on the Gestures page, every key on the Keys page;
  // - Read Pico has no Confirm key for the long-press function, and plugins / SD
  //   firmware update are not part of RickyOS (Profile is the card on top of Settings).
  const auto elsewhere = [](const SettingInfo& setting) {
    const auto field = setting.valuePtr;
    if (field == &CrossPointSettings::sleepScreen || field == &CrossPointSettings::standbyOverlay ||
        field == &CrossPointSettings::quickResumeSleepScreen || field == &CrossPointSettings::sleepScreenCoverMode ||
        field == &CrossPointSettings::sleepScreenCoverFilter || field == &CrossPointSettings::sleepTimeoutMinutes) {
      return true;
    }
    return field == &CrossPointSettings::touchReaderControls || field == &CrossPointSettings::pageTurnGesture ||
           field == &CrossPointSettings::previousPageGesture || field == &CrossPointSettings::showReaderMenu ||
           field == &CrossPointSettings::pageTurnDirection || field == &CrossPointSettings::sideButtonLayout ||
           field == &CrossPointSettings::longPressButtonBehavior ||
           field == &CrossPointSettings::standbyShortcutEnabled || field == &CrossPointSettings::shortPwrBtn ||
           field == &CrossPointSettings::pwrBtnFootnoteBack || field == &CrossPointSettings::longPressMenuFunction ||
           setting.action == SettingAction::SdFirmwareUpdate;
  };
  for (auto* list : {&displaySettings, &readerSettings, &controlsSettings, &systemSettings}) {
    removeMatching(*list, elsewhere);
  }

  // Standby & power-off lives in Apps (one entry); Display keeps the display rows.
  // Network & Sync.
  moveMatching(systemSettings, connectionSettings, [](const SettingInfo& setting) {
    return setting.action == SettingAction::Network || setting.action == SettingAction::KOReaderSync;
  });
  moveMatching(controlsSettings, connectionSettings,
               [](const SettingInfo& setting) { return setting.action == SettingAction::Bluetooth; });
  // Fonts.
  moveMatching(readerSettings, fontSettings,
               [](const SettingInfo& setting) { return setting.action == SettingAction::DownloadFonts; });
  // Library & Files: how books are listed and found, and the reading cache.
  moveMatching(systemSettings, librarySettings, [](const SettingInfo& setting) {
    return setting.valuePtr == &CrossPointSettings::libraryUseMetadata ||
           setting.valuePtr == &CrossPointSettings::removeReadBooksFromRecents ||
           setting.valuePtr == &CrossPointSettings::moveFinishedToReadFolder ||
           setting.valuePtr == &CrossPointSettings::showHiddenFiles || setting.action == SettingAction::ClearCache;
  });
  // System: Gestures and Keys first, then what is left.
  systemSettings.reserve(systemSettings.size() + controlsSettings.size() + 2);
  moveMatching(controlsSettings, systemSettings, [](const SettingInfo&) { return true; });
  systemSettings.insert(systemSettings.begin(), SettingInfo::Action(StrId::STR_RICKY_KEYS, SettingAction::RickyKeys));
  systemSettings.insert(systemSettings.begin(),
                        SettingInfo::Action(StrId::STR_RICKY_GESTURES, SettingAction::RickyGestures));
  for (auto& setting : displaySettings) {
    if (setting.valuePtr == &CrossPointSettings::sleepScreenCoverMode) {
      setting.nameId = StrId::STR_RICKY_COVER_FIT;
    } else if (setting.valuePtr == &CrossPointSettings::sleepScreenCoverFilter) {
      setting.nameId = StrId::STR_RICKY_COVER_FILTER;
    }
  }
}

// Rows that lead somewhere (a page, a picker, a slider, the keyboard) carry a chevron;
// switches and in-place actions (clear cache, restore) do not.
bool SettingsActivity::opensPage(const SettingInfo& setting) {
  switch (setting.type) {
    case SettingType::TOGGLE:
      return false;
    case SettingType::ENUM: {
      const auto labels = setting.enumLabels();
      const bool onOff = setting.enumStringValues.empty() && labels.size() == 2 && labels[0] == StrId::STR_STATE_OFF &&
                         labels[1] == StrId::STR_STATE_ON;
      return !onOff;
    }
    case SettingType::ACTION:
      return setting.action != SettingAction::ClearCache && setting.action != SettingAction::RestoreSystemSettings;
    case SettingType::VALUE:
    case SettingType::STRING:
      return true;
  }
  return false;
}

const char* SettingsActivity::rickySettingDescription(const SettingInfo& setting) {
  if (setting.valuePtr == &CrossPointSettings::sleepScreen) return tr(STR_RICKY_HELP_LOCK);
  if (setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen) return tr(STR_RICKY_HELP_KEEP_PAGE);
  if (setting.valuePtr == &CrossPointSettings::sleepTimeoutMinutes) return tr(STR_RICKY_HELP_TIMEOUT);
  if (setting.valuePtr == &CrossPointSettings::rickyAutoOffIndex) return tr(STR_RICKY_HELP_AUTO_OFF);
  if (setting.valuePtr == &CrossPointSettings::shortPwrBtn) return tr(STR_RICKY_HELP_POWER);
  if (setting.valuePtr == &CrossPointSettings::refreshFrequency) return tr(STR_RICKY_HELP_REFRESH);
  if (setting.action == SettingAction::RestoreSystemSettings) return tr(STR_RICKY_HELP_RESET);
  if (setting.action == SettingAction::AppVisibility) return tr(STR_RICKY_HELP_APPS);
  if (setting.action == SettingAction::RickyScreenRepair) return tr(STR_RICKY_HELP_SCREEN_REPAIR);
  return nullptr;
}

void SettingsActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  if (usesAccordion()) {
    moveSelectionTo(MainTabs::contentEdgeIndex(edge, listCount()));
    return;
  }
  moveRingTo(MainTabs::contentEdgeIndex(edge, settingsCount) + (settingsCount > 0 ? 1 : 0));
}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();
  librarySettings.clear();
  connectionSettings.clear();
  fontSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  std::vector<DictionaryEntry> dictionaries;
  if (!usesAccordion() || dictionariesLoaded) DictionaryRegistry::discover(dictionaries);

  // getSettingsList() returns a temporary that this loop owns, and no element is
  // read after it is filed, so move rather than deep-copy. SettingInfo owns two
  // vectors, a span and four std::function members (~144 B), so copying each
  // selected entry re-allocates all of that storage on every rebuild.
  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT || home_button::isSetting(setting.valuePtr)) continue;
    if (!usesAccordion() && (setting.valuePtr == &CrossPointSettings::inxRecentLayout ||
                             setting.valuePtr == &CrossPointSettings::inxLibraryLayout ||
                             setting.valuePtr == &CrossPointSettings::inxAppsLayout ||
                             setting.valuePtr == &CrossPointSettings::inxTabPosition)) {
      continue;
    }
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      // The sunlight fading fix is a grayscale-waveform compensation that does
      // not apply on the X4 Pro / X4 Classic (plain OTP waveform, same panels).
      if (setting.valuePtr == &CrossPointSettings::fadingFix && (BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC)) {
        continue;
      }
      displaySettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings || setting.inReadingStatsSettings) continue;
      readerSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (BoardConfig::hasHomeKey() && setting.valuePtr == &CrossPointSettings::longPressMenuFunction) continue;
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      // These stay in the shared list for persistence and the web API, but the
      // device UI owns them in the Date & Time submenu.
      if (setting.valuePtr == &CrossPointSettings::clockAutoSync ||
          setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ ||
          setting.valuePtr == &CrossPointSettings::clockFormat) {
        continue;
      }
      systemSettings.push_back(std::move(setting));
    }
  }

  // Append device-only ACTION items
  if (!BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }
#if FREEINK_CAP_BLE_HID_HOST
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_BLUETOOTH, SettingAction::Bluetooth));
#endif
  systemSettings.push_back(SettingInfo::Action(StrId::STR_APP_VISIBILITY, SettingAction::AppVisibility));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DATE_AND_TIME, SettingAction::ClockSettings));
#if !FREEINK_DEVICE_METALIO_PLUS
  if (BoardConfig::hasHomeKey()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_HOME_BUTTON, SettingAction::HomeButton));
  }
#endif
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_RESTORE_SYSTEM_SETTINGS, SettingAction::RestoreSystemSettings));
  // The product uses only its owned, accepted stable channel. Other targets
  // retain the upstream proxy and release-channel UI.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_RICKYOS_FIRMWARE_UPDATE, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_RICKY_SCREEN_REPAIR, SettingAction::RickyScreenRepair));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  if (usesAccordion()) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_ABOUT, SettingAction::About));
  } else {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_ABOUT, SettingAction::About));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  }
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.insert(readerSettings.begin() + 2,
                        SettingInfo::Action(StrId::STR_MANAGE_DICTIONARIES, SettingAction::ManageDictionaries));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_READING_STATS, SettingAction::ReadingStatsSettings));

  reorganizeRickySettings();
  currentSettings = &settingsForCategory(selectedCategoryIndex);
  settingsCount = static_cast<int>(currentSettings->size());
  rebuildRowItems();
}

void SettingsActivity::onEnter() {
  RenderLock lock(*this);
  UiTabListActivity::onEnter();

  // Reset selection to first category (ring position 0, the tab bar, comes
  // from the base's per-tab nav reset)
  selectedCategoryIndex = 0;
  expandedCategories = 0;
  categoryRoot_ = true;
  categoryNav_.reset();
  app.on(
      ACTION_TAB_USER + 20,
      [](const fui::ActionEvent&, void* user) {
        auto& self = *static_cast<SettingsActivity*>(user);
        self.app.clearTapFlash();
        self.startActivityForResultWith<RickyProfileActivity>([](const ActivityResult&) {});
      },
      this);
  app.on(
      ACTION_TAB_USER + 21,
      [](const fui::ActionEvent&, void* user) {
        auto& self = *static_cast<SettingsActivity*>(user);
        self.app.clearTapFlash();
        self.startActivityForResultWith<AboutActivity>([](const ActivityResult&) {});
      },
      this);
  dictionariesLoaded = !usesAccordion();
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  currentSettings = &settingsForCategory(selectedCategoryIndex);
  settingsCount = static_cast<int>(currentSettings->size());
  activeNav().top = 0;  // category switches start the list at the top (no per-tab memory here)
  rebuildRowItems();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for *currentSettings.
// Structural — call only when the active category or a category's setting
// list changes, never from buildScreen(), which only refreshes rowValues_
// content and rowItems_[].value pointers in place.
void SettingsActivity::rebuildRowItems() {
  const size_t count = static_cast<size_t>(listCount());
  rowValues_.resize(count);
  rowItems_.clear();
  rowItems_.reserve(count);  // Existing storage; bounded by the current category.
  for (size_t i = 0; i < count; ++i) {
    fui::ListItem item;
    item.label = categoryRoot_ ? (i < categoryCount    ? I18N.get(categoryNames[i])
                                  : i == categoryCount ? tr(STR_RICKY_ABOUT_DEVICE)
                                                       : tr(STR_RICKY_HELP_FEEDBACK))
                               : I18N.get((*currentSettings)[i].nameId);
    item.subtitle = categoryRoot_ ? (i < categoryCount ? I18N.get(categoryDescriptions[i]) : nullptr)
                                  : rickySettingDescription((*currentSettings)[i]);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
  return;
}

int SettingsActivity::listCount() const { return categoryRoot_ ? categoryCount + 2 : settingsCount; }

freeink::ui::ListNav& SettingsActivity::activeNav() { return nav; }

void SettingsActivity::openRickyCategory(const int index) {
  if (index < 0 || index >= categoryCount) return;
  RenderLock lock(*this);
  closeRouting();
  categoryNav_ = nav;
  categoryRoot_ = false;
  selectedCategoryIndex = index;
  if (index == 1 && !dictionariesLoaded) {
    dictionariesLoaded = true;
    rebuildSettingsLists();
  }
  selectCategory(index);
  nav.reset();
  requestUpdate();
}

void SettingsActivity::backToRickyCategories() {
  RenderLock lock(*this);
  closeRouting();
  categoryRoot_ = true;
  nav = categoryNav_;
  rebuildRowItems();
  requestUpdate();
}

void SettingsActivity::rebuildAccordionRows() {
  const auto counts = accordionSettingCounts();
  const int count = InxAccordionGeometry::visibleCount(counts, expandedCategories);
  rowLabels_.resize(static_cast<size_t>(count));
  rowValues_.resize(static_cast<size_t>(count));
  rowItems_.clear();
  rowItems_.reserve(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    const auto row = InxAccordionGeometry::rowAt(counts, expandedCategories, index);
    const bool category = row.isCategory();
    rowLabels_[index] = category ? I18N.get(categoryNames[row.category])
                                 : std::string("  ") + I18N.get(settingsForCategory(row.category)[row.setting].nameId);
    fui::ListItem item;
    item.label = rowLabels_[index].c_str();
    item.subtitle = category ? I18N.get(categoryDescriptions[row.category])
                             : rickySettingDescription(settingsForCategory(row.category)[row.setting]);
    item.actionValue = static_cast<int16_t>(index);
    if (category) item.state = fui::StateEmphasized;
    rowItems_.push_back(item);
  }
}

void SettingsActivity::onTabAction(const int index) {
  if (optionPopup.isActive()) return;
  selectCategory(index);
  activeNav().selected = 0;  // tab taps land with the tab bar focused
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (categoryRoot_) {
    if (index == categoryCount) {
      startActivityForResultWith<AboutActivity>([](const ActivityResult&) {});
      return;
    }
    if (index == categoryCount + 1) {
      startActivityForResultWith<RickyHelpActivity>([](const ActivityResult&) {});
      return;
    }
    if (index == 3) {
      releaseListsForMemoryHungryChild();
      if (!startActivityForResultWith<FontLibraryActivity>([this](const ActivityResult&) {
            rebuildSettingsLists();
            requestUpdate();
          })) {
        rebuildSettingsLists();
        requestUpdate();
      }
      return;
    }
    openRickyCategory(index);
  } else {
    // Existing action/value bindings read a one-based per-category ring.
    tabNavs[selectedCategoryIndex].selected = index + 1;
    toggleCurrentSetting();
    requestUpdate();
  }
}

void SettingsActivity::onRowAction(const fui::ActionEvent& event) {
  UiListActivity::onRowAction(event);
  return;
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme changes take effect immediately, on this screen — reload the theme
  // and re-derive the app's tokens so the very next repaint is in the new look.
  if (valuePtr != &CrossPointSettings::uiTheme) {
    return;
  }
  RenderLock lock(*this);
  UITheme::getInstance().reload();
  expandedCategories = 0;
  nav.reset();
  rebuildSettingsLists();
  // Re-derive the shared tokens for the new look; the gate stays closed until
  // the repaint that rebuilds the interaction table in the new layout.
  resetUi();
}

bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SettingsActivity::navigateButtons() {
  if (usesAccordion()) {
    UiListActivity::navigateButtons();
    return;
  }
  UiTabListActivity::navigateButtons();
}

void SettingsActivity::stepTab(const int direction) {
  // Ring position 0 stays on the tab bar; a row selection collapses to the
  // new category's first row (per-tab memory is deliberately not kept here).
  const bool onTabBar = ringPos() == 0;
  selectedCategoryIndex = direction > 0 ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                                        : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
  selectCategory(selectedCategoryIndex);
  activeNav().selected = onTabBar ? 0 : 1;
  requestUpdate();
}

bool SettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!categoryRoot_)
      backToRickyCategories();
    else if (!usesMainTabBar()) {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return true;
  }
  return false;
}

bool SettingsActivity::usesAccordion() const { return true; }

const std::vector<SettingInfo>& SettingsActivity::settingsForCategory(const int categoryIndex) const {
  switch (categoryIndex) {
    case 0:
      return displaySettings;
    case 1:
      return readerSettings;
    case 2:
      return connectionSettings;
    case 3:
      return fontSettings;
    case 4:
      return librarySettings;
    case 5:
      return systemSettings;
  }
  return displaySettings;
}

std::array<int, SettingsActivity::categoryCount> SettingsActivity::accordionSettingCounts() const {
  return {static_cast<int>(displaySettings.size()),    static_cast<int>(readerSettings.size()),
          static_cast<int>(connectionSettings.size()), static_cast<int>(fontSettings.size()),
          static_cast<int>(librarySettings.size()),    static_cast<int>(systemSettings.size())};
}

void SettingsActivity::toggleAccordionCategory(const int categoryIndex) {
  if (categoryIndex < 0 || categoryIndex >= categoryCount) return;
  const uint8_t mask = static_cast<uint8_t>(uint8_t{1} << categoryIndex);
  if (categoryIndex == 1 && !dictionariesLoaded && (expandedCategories & mask) == 0) {
    dictionariesLoaded = true;
    rebuildSettingsLists();
  }
  // One open group at a time keeps the small-screen list understandable.
  expandedCategories = (expandedCategories & mask) ? 0 : mask;
  rebuildAccordionRows();
  nav.selected = InxAccordionGeometry::categoryRow(accordionSettingCounts(), expandedCategories, categoryIndex);
  nav.follow(listCount());
  requestUpdate();
}

void SettingsActivity::toggleAccordionSetting(const int categoryIndex, const int settingIndex) {
  if (categoryIndex < 0 || categoryIndex >= categoryCount) return;
  const auto& settings = settingsForCategory(categoryIndex);
  if (settingIndex < 0 || settingIndex >= static_cast<int>(settings.size())) return;
  selectedCategoryIndex = categoryIndex;
  currentSettings = &settings;
  settingsCount = static_cast<int>(settings.size());
  tabNavs[static_cast<size_t>(categoryIndex)].selected = settingIndex + 1;
  toggleCurrentSetting();
  if (!usesAccordion()) return;
  rebuildAccordionRows();
  nav.selected =
      InxAccordionGeometry::categoryRow(accordionSettingCounts(), expandedCategories, categoryIndex) + 1 + settingIndex;
  nav.follow(listCount());
  requestUpdate();
}

void SettingsActivity::toggleCurrentSetting() {
  mappedInput.resetHomeButtonInput();
  int selectedSetting = ringPos() - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const auto changedValuePtr = setting.valuePtr;
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const auto enumLabels = setting.enumLabels();
    bool showPicker = enumLabels.size() > 2;
    const bool switchLabels =
        enumLabels.size() == 2 && enumLabels[0] == StrId::STR_STATE_OFF && enumLabels[1] == StrId::STR_STATE_ON;
    showPicker = enumLabels.size() > 1 && !switchLabels;
    if (showPicker) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(
          setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), currentValue,
          [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
            if (SETTINGS.*valuePtr == idx) return;
            SETTINGS.*valuePtr = idx;
#if FREEINK_CAP_HAPTIC && !defined(SIMULATOR)
            if (valuePtr == &CrossPointSettings::hapticFeedbackLevel && idx == CrossPointSettings::HAPTIC_FEEDBACK_OFF)
              gpio.stopHapticFeedback();
#endif
            syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
            SETTINGS.saveToFile();
            if (valuePtr == &CrossPointSettings::uiTheme)
              applyUiSettingChange(valuePtr);
            else
              rebuildSettingsLists();
          });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(enumLabels.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumLabels().size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    bool showPicker = totalValues > 2 || setting.managedEnumPicker;
    const auto enumLabels = setting.enumLabels();
    const bool switchLabels =
        enumLabels.size() == 2 && enumLabels[0] == StrId::STR_STATE_OFF && enumLabels[1] == StrId::STR_STATE_ON;
    showPicker = showPicker || (totalValues > 1 && !switchLabels);
    if (showPicker) {
      const auto valueSetter = setting.valueSetter;
      const bool managedPicker = setting.managedEnumPicker;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged, managedPicker,
                       cur](const int idx) {
        if (idx == cur) return;
        valueSetter(static_cast<uint8_t>(idx));
        if (managedPicker) return;
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        const auto enumLabels = setting.enumLabels();
        optionPopup.show(setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    // Every child page returns here through ActivityManager's Pop; make sure the
    // settings list is both rebuilt (values may have changed) and repainted.
    auto resultHandler = [this](const ActivityResult&) {
      SETTINGS.saveToFile();
      rebuildSettingsLists();
      requestUpdate();
    };

    switch (setting.action) {
      case SettingAction::HomeButton:
#if FREEINK_DEVICE_METALIO_PLUS
      case SettingAction::RickyKeys:
#endif
      {
        // Activities must outlive this call and are owned by the activity stack.
        auto activity = makeUniqueNoThrow<HomeButtonSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "OOM: Home button settings");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      case SettingAction::RemapFrontButtons:
        startActivityForResultWith<ButtonRemapActivity>(resultHandler);
        break;
      case SettingAction::Bluetooth:
#if FREEINK_CAP_BLE_HID_HOST
        releaseListsForMemoryHungryChild();
        if (!startActivityForResultWith<BluetoothSettingsActivity>(resultHandler)) {
          rebuildSettingsLists();
          requestUpdate();
        }
#endif
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResultWith<StatusBarSettingsActivity>(resultHandler);
        break;
      case SettingAction::ReadingStatsSettings:
        startActivityForResultWith<ReadingStatsSettingsActivity>(resultHandler);
        break;
      case SettingAction::AppVisibility:
        startActivityForResultWith<AppVisibilitySettingsActivity>(resultHandler);
        break;
      case SettingAction::ClockSettings:
        if (auto activity = makeUniqueNoThrow<ClockSettingsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: ClockSettingsActivity");
        }
        break;
      case SettingAction::KOReaderSync:
        startActivityForResultWith<KOReaderSettingsActivity>(resultHandler);
        break;
      case SettingAction::Network: {
        auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
        if (!activity) {
          LOG_ERR("SETTINGS", "OOM: WifiSelectionActivity");
          return;
        }
        startActivityForResult(std::move(activity), [](const ActivityResult&) {
          SETTINGS.saveToFile();
          // Every other WiFi consumer hands the radio to a session it owns;
          // these rows only save credentials, so nothing here would ever
          // release the driver's heap. The scan alone brings it up, so tear
          // down whether or not the user joined a network.
          if (WiFi.getMode() == WIFI_MODE_NULL) return;
          WiFi.disconnect(false);
          delay(30);
          // Unlike the onExit() teardowns, this runs from the loop task with
          // no lock held; the restart popup paints straight to the panel.
          RenderLock lock;
          silentRestartToSettings();
        });

        break;
      }
      case SettingAction::ClearCache:
        startActivityForResultWith<ClearCacheActivity>(resultHandler);
        break;
      case SettingAction::RickyGestures:
        startActivityForResultWith<RickyOptionListActivity>(resultHandler, RickyOptionLists::gestures());
        break;
#if !FREEINK_DEVICE_METALIO_PLUS
      case SettingAction::RickyKeys:
        startActivityForResultWith<RickyOptionListActivity>(resultHandler, RickyOptionLists::keys());
        break;
#endif
      case SettingAction::RickyScreenRepair: {
        // Residual charge in the film shows old screens through new ones. Full GC16
        // black/white pairs are DC-balanced and drive every pixel through both
        // extremes, which is the usual way to bring a stressed panel back.
        constexpr int kCycles = 5;
        RenderLock lock;
#if !defined(SIMULATOR)
        // A scan path that drifted out of step draws every frame with a shifted copy and
        // survives full refreshes; rebuilding it first is what a reboot used to fix.
        display.restartPanel();
#endif
        GUI.drawPopup(renderer, tr(STR_RICKY_SCREEN_REPAIRING));
        for (int i = 0; i < kCycles; ++i) {
          renderer.clearScreen(0x00);
          renderer.displayBuffer(HalDisplay::FULL_REFRESH);
          renderer.clearScreen(0xFF);
          renderer.displayBuffer(HalDisplay::FULL_REFRESH);
        }
        renderer.requestNextFullRefresh();
        requestUpdate();
        break;
      }
      case SettingAction::RickyProfile:
        startActivityForResultWith<RickyProfileActivity>(resultHandler);
        break;
      case SettingAction::RestoreSystemSettings:
        confirmRestoreSystemSettings();
        break;
      case SettingAction::CheckForUpdates:
        openOtaUpdate();
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResultWith<SdFirmwareUpdateActivity>(resultHandler);
        break;
      case SettingAction::DownloadFonts:
        releaseListsForMemoryHungryChild();
        if (!startActivityForResultWith<FontLibraryActivity>(resultHandler)) {
          rebuildSettingsLists();
          requestUpdate();
        }
        break;
      case SettingAction::ManageDictionaries:
        startActivityForResultWith<DictionaryDownloadActivity>([this](const ActivityResult&) {
          SETTINGS.saveToFile();
          rebuildSettingsLists();
          requestUpdate();
        });
        break;
      case SettingAction::TextSettings:
        startActivityForResultWith<TextSettingsActivity>(
            [this](const ActivityResult&) {
              // TextSettingsActivity saves on each change; no save needed here.
              {
                RenderLock lock(*this);
                sdFontSystem.ensureLoaded(renderer);
              }
              rebuildSettingsLists();
              requestUpdate();
            },
            &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildRowItems() and don't
        // re-run on Pop (see ActivityManager::loop()), so a language switch
        // needs an explicit rebuild here rather than the generic resultHandler.
        startActivityForResultWith<LanguageSelectActivity>([this](const ActivityResult&) {
          SETTINGS.saveToFile();
          rebuildSettingsLists();
        });
        break;
      case SettingAction::About:
        if (usesAccordion())
          startActivityForResultWith<InxAboutActivity>(resultHandler);
        else
          startActivityForResultWith<AboutActivity>(resultHandler);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  if (changedValuePtr == &CrossPointSettings::uiTheme) {
    applyUiSettingChange(changedValuePtr);
  } else {
    rebuildSettingsLists();
    activeNav().selected = std::clamp(selectedSetting, 0, std::max(0, settingsCount - 1));
  }
}

void SettingsActivity::confirmRestoreSystemSettings() {
  const bool started = startActivityForResultWith<ConfirmationActivity>(
      [this](const ActivityResult& firstResult) {
        if (firstResult.isCancelled) return;

        const bool finalStarted = startActivityForResultWith<ConfirmationActivity>(
            [this](const ActivityResult& finalResult) {
              if (finalResult.isCancelled) return;
              if (systemSettingsReset::clearPersistedSettings()) {
                silentRestart();
                return;
              }
              optionPopup.show(StrId::STR_RESTORE_SYSTEM_SETTINGS_FAILED, OK_OPTION,
                               static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
              requestUpdate();
            },
            tr(STR_RESTORE_SYSTEM_SETTINGS), tr(STR_RESTORE_SYSTEM_SETTINGS_FINAL_WARNING));
        if (finalStarted) return;
        optionPopup.show(StrId::STR_MEMORY_ERROR, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
        requestUpdate();
      },
      tr(STR_RESTORE_SYSTEM_SETTINGS), tr(STR_RESTORE_SYSTEM_SETTINGS_WARNING));
  if (started) return;
  optionPopup.show(StrId::STR_MEMORY_ERROR, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
  requestUpdate();
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::releaseListsForMemoryHungryChild() {
  RenderLock lock(*this);
  closeRouting();
  // clear() keeps vector capacity; swap it out so a stacked Settings activity
  // leaves that heap to the child.
  std::vector<freeink::ui::ListItem>().swap(rowItems_);
  std::vector<std::string>().swap(rowValues_);
  std::vector<std::string>().swap(rowLabels_);
  std::vector<SettingInfo>().swap(displaySettings);
  std::vector<SettingInfo>().swap(readerSettings);
  std::vector<SettingInfo>().swap(controlsSettings);
  std::vector<SettingInfo>().swap(systemSettings);
  std::vector<SettingInfo>().swap(librarySettings);
  std::vector<SettingInfo>().swap(connectionSettings);
  std::vector<SettingInfo>().swap(fontSettings);
  settingsCount = 0;
}

void SettingsActivity::openOtaUpdate() {
  releaseListsForMemoryHungryChild();

  if (startActivityForResultWith<OtaUpdateActivity>([this](const ActivityResult&) {
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      })) {
    return;
  }

  rebuildSettingsLists();
  requestUpdate();
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResultWith<IntervalSelectionActivity>(
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      },
      "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
      CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
      StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER);
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.action == SettingAction::HomeButton) return tr(STR_CONFIGURE);
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = setting.valuePtr == &CrossPointSettings::uiTheme &&
                                  SETTINGS.uiTheme == CrossPointSettings::COVER_GRID && !UITheme::supportsCoverGrid()
                              ? CrossPointSettings::LYRA
                              : SETTINGS.*(setting.valuePtr);
    const auto enumLabels = setting.enumLabels();
    if (value >= enumLabels.size()) return "";
    return I18N.get(enumLabels[value]);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    const auto enumLabels = setting.enumLabels();
    if (value < enumLabels.size()) {
      return I18N.get(enumLabels[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  return "";
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto applyCheckbox = [](const SettingInfo& setting, fui::ListItem& item) {
    item.toggle = false;
    const auto labels = setting.enumLabels();
    const bool checkbox = setting.type == SettingType::TOGGLE ||
                          (setting.type == SettingType::ENUM && setting.enumStringValues.empty() &&
                           labels.size() == 2 && labels[0] == StrId::STR_STATE_OFF && labels[1] == StrId::STR_STATE_ON);
    if (checkbox && (setting.valuePtr || setting.valueGetter)) {
      const bool checked = setting.valuePtr ? SETTINGS.*(setting.valuePtr) != 0 : setting.valueGetter() != 0;
      GUI.setCheckboxRow(item, checked);
    }
  };
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Do not thicken every glyph in the list (including small help text).
  // Category emphasis uses the list's font style; reading bold stays independent.
  constexpr bool boldChineseCategories = false;
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});

  if (categoryRoot_) {
    const auto& theme = screen.theme();
    auto& target = screen.target();
    screen.insetContent(fui::Insets{0, ricky::ds::space::kPageMargin, 0, ricky::ds::space::kPageMargin});
    const int gap = ricky::ds::space::kGutter;
    const int pad = ricky::ds::space::kCardPadding;
    const int bodyHeight = target.lineHeight(theme.bodyText.font);
    const int smallHeight = target.lineHeight(theme.smallText.font);
    auto pageTitle = theme.titleText;
    pageTitle.bold = false;
    target.text(screen.takeTop(ricky::ds::layout::kPageHeaderHeight, 0), tr(STR_SETTINGS_TITLE), pageTitle);
    // Profile card, then the categories as Boox-style icon cards in two columns.
    const int profileHeight = ricky::ds::layout::kProfileCardHeight;
    const int columns = content.width > content.height ? 3 : 2;
    const int rows = (categoryCount + columns - 1) / columns;
    const int cellHeight = ricky::ds::layout::kSettingsCardHeight;
    const int section = ricky::ds::space::k8;
    RickyProfile::drawCard(screen, renderer, screen.takeTop(profileHeight, section), ACTION_TAB_USER + 20);
    const int selected = RickyPageUi::syncNav(nav, listCount());
    const bool focus = showMainTabContentSelection();
    const freeink::Icon* icons[] = {&icon_ricky_display_40, &icon_ricky_reader_40, &icon_ricky_network_40,
                                    &icon_ricky_fonts_40,   &icon_ricky_folder_40, &icon_ricky_system_40};
    static_assert(sizeof(icons) / sizeof(icons[0]) == categoryCount);
    const auto grid = screen.takeTop(cellHeight * rows + gap * (rows - 1), 0);
    auto label = theme.bodyText;
    label.maxLines = 1;
    for (int i = 0; i < categoryCount; ++i) {
      const auto rect = RickyPageUi::uiRect(
          RickyPageLayout::cell(Rect{grid.x, grid.y, grid.width, grid.height}, i, categoryCount, gap, columns));
      const bool picked = focus && selected == i;
      RickyPageUi::card(target, rect, picked);
      screen.frame().hit(rect, ACTION_ROW, i, fui::InputTouch);
      RickyPageUi::pageIcon(target, renderer, rect.x + pad, rect.y + pad + icons[i]->h / 2, *icons[i], picked);
      RickyDesignUi::text(renderer,
                          {static_cast<int16_t>(rect.x + pad), static_cast<int16_t>(rect.y + 80),
                           static_cast<int16_t>(rect.width - pad * 2), 44},
                          I18N.get(categoryNames[i]), RickyDesignUi::HEADLINE, picked);
    }
    screen.takeTop(section, 0);
    const auto about = screen.takeTop(ricky::ds::layout::kRowHeight, ricky::ds::space::kTouchSpacing);
    const bool aboutPicked = focus && selected == categoryCount;
    if (aboutPicked) RickyDesignUi::fill(renderer, about, ricky::ds::gray::kInk, ricky::ds::radius::kControl);
    RickyDesignUi::text(renderer, about, tr(STR_RICKY_ABOUT_DEVICE), RickyDesignUi::BODY, aboutPicked);
    const fui::Rect version{static_cast<int16_t>(about.right() - 248), about.y, 208, about.height};
    RickyDesignUi::secondaryText(renderer, version, CROSSPOINT_VERSION, RickyDesignUi::CAPTION, aboutPicked,
                                 fui::TextAlign::Right);
    RickyPageUi::chevron(target, {static_cast<int16_t>(about.right() - 24), about.y, 24, about.height}, aboutPicked);
    screen.frame().hit(about, ACTION_ROW, categoryCount, fui::InputTouch);
    const auto help = screen.takeTop(ricky::ds::layout::kRowHeight, 0);
    const bool helpPicked = focus && selected == categoryCount + 1;
    if (helpPicked) RickyDesignUi::fill(renderer, help, ricky::ds::gray::kInk, ricky::ds::radius::kControl);
    RickyDesignUi::text(renderer, help, tr(STR_RICKY_HELP_FEEDBACK), RickyDesignUi::BODY, helpPicked);
    RickyPageUi::chevron(target, {static_cast<int16_t>(help.right() - 24), help.y, 24, help.height}, helpPicked);
    screen.frame().hit(help, ACTION_ROW, categoryCount + 1, fui::InputTouch);
    return;
  }
  for (size_t i = 0; i < rowItems_.size(); ++i) {
    auto& item = rowItems_[i];
    item.toggle = false;
    rowValues_[i] = categoryRoot_ ? "" : settingValueText((*currentSettings)[i]);
    item.value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    if (!categoryRoot_) applyCheckbox((*currentSettings)[i], item);
  }
  fui::ListProps productProps;
  productProps.items = rowItems_.data();
  productProps.count = static_cast<uint16_t>(rowItems_.size());
  productProps.action = ACTION_ROW;
  productProps.inputMask = fui::InputTouch;
  productProps.labelText = screen.theme().bodyText;
  productProps.labelText.maxLines = 2;
  productProps.subtitleText = screen.theme().smallText;
  productProps.subtitleText.color = fui::Color::DarkGray;
  productProps.subtitleText.maxLines = 2;
  productProps.rowGap = std::max<int16_t>(6, screen.theme().listRowGap);
  const bool designRows = UiHighDpiProfile::enabled && !categoryRoot_;
  if (designRows) RickyRowList::prepare(screen, productProps);
  syncListViewport(screen, productProps);
  if (usesMainTabBar() && !showMainTabContentSelection()) productProps.selectedIndex = -1;
  GfxRenderer::SyntheticBoldScope productBold(renderer, CrossPointSettings::SYNTHETIC_BOLD_OFF);
  if (designRows) {
    // System is a quiet settings list. Repeating disclosure arrows on nearly
    // every entry made a second visual rail; keep values/toggles and full-row
    // actions, while other settings retain their existing disclosure policy.
    const bool quietSystem = currentSettings == &systemSettings;
    RickyRowList::draw(screen, renderer, nav, productProps, [this, quietSystem](const int i) {
      RickyRowList::RowStyle style;
      style.chevron = !quietSystem && i < settingsCount && opensPage((*currentSettings)[i]);
      return style;
    });
    return;
  }
  screen.list(productProps);
  return;
}

void SettingsActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  drawPageHeader(Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 categoryRoot_ ? tr(STR_SETTINGS_TITLE) : I18N.get(categoryNames[selectedCategoryIndex]),
                 categoryRoot_ ? CROSSPOINT_VERSION : nullptr);
}

void SettingsActivity::drawFooter() {
  const auto productLabels = mainTabButtonLabels(tr(STR_BACK), tr(STR_SELECT), listCount() > 1);
  GUI.drawButtonHints(renderer, productLabels.btn1, productLabels.btn2, productLabels.btn3, productLabels.btn4);
  return;
}

void SettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
