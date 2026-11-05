#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalAudioOutput.h>
#include <HalClock.h>
#include <array>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "ReadingStatsStore.h"
#include "util/TimeUtils.h"

#include "activities/ActivityManager.h"
#include "activities/apps/standby/StandbyActivity.h"
#include "components/RickyPageUi.h"
#include "components/icons/rickyPageIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// The sheet uses RickyOS card/control radii, with 88px FUI touch bands.
// Geometry and hit testing share the same emitted FUI rectangles.
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;
constexpr int kVolumeStep = 101;
constexpr int kVolumePreview = 200;
constexpr int kVolumeDrag = 203;  // ACTION_TILE value of the volume track
constexpr int16_t kGrabberHeight = 5;  // fui::SheetProps default, mirrored here
// Light and volume rows: a head line (name, then the switch or the sound test at the
// right) and a band of [-] [track] [+]. Every control sits in an 88 px touch square.
constexpr int16_t kTouch = ricky::ds::layout::kTouchMin;
constexpr int16_t kHeadHeight = 64;  // the switch and the sound test still get 88 px touch boxes
constexpr int kHeadNone = 0, kHeadSwitch = 1, kHeadPreview = 2;
constexpr int16_t kSwitchWidth = 80;
constexpr int16_t kSwitchHeight = 44;
constexpr int16_t kPreviewSize = 60;
// Cards replace the old full-width rules: one for the light, one for the sound.
constexpr int16_t kCardPad = 16;
constexpr uint8_t kCardRadius = ricky::ds::radius::kContainer;
constexpr int16_t kGroupGap = 20;   // between the tiles and the cards, and card to card
constexpr int16_t kPillHeight = 64; // drawn pill; its touch band is the full kTouch
constexpr int16_t kBandGap = 8;     // between two pills in one card
// A head's control box reaches (kTouch - kHeadHeight) / 2 below the head; a band
// right under it starts this much lower so the boxes keep 8 px.
constexpr int16_t kBareBandGap = (kTouch - kHeadHeight) / 2 + 8;
constexpr auto kPlayBits = [] {
  std::array<uint8_t, 128> bits{};
  for (auto& b : bits) b = 0xff;
  for (int y = 6; y < 26; ++y) {
    const int extent = 20 - 2 * (y < 16 ? 16 - y : y - 16);
    for (int x = 9; x < 9 + extent; ++x) bits[y * 4 + x / 8] &= ~(0x80 >> (x % 8));
  }
  return bits;
}();
constexpr freeink::Icon kPlayIcon{32, 32, 16, kPlayBits.data()};
// A speaker for the volume pill: a small box and its widening cone (cleared bits = ink).
constexpr auto kSpeakerBits = [] {
  std::array<uint8_t, 128> bits{};
  for (auto& b : bits) b = 0xff;
  for (int x = 5; x < 24; ++x) {
    const int half = x < 12 ? 5 : 5 + (x - 12);
    for (int y = 16 - half; y < 16 + half; ++y) bits[y * 4 + x / 8] &= ~(0x80 >> (x % 8));
  }
  return bits;
}();
constexpr freeink::Icon kSpeakerIcon{32, 32, 16, kSpeakerBits.data()};

// Takes a rounded card of `height` off the top of `screen`; returns a screen laid
// out inside its padding, as the landscape columns do.
using CardScreen = UiAppHost::UiScreen;
CardScreen cardArea(CardScreen& screen, const int16_t height) {
  const auto rect = screen.takeTop(height, 0);
  screen.target().stroke(rect, fui::Paint::solid(fui::Color::DarkGray), ricky::ds::stroke::kHairline, kCardRadius);
  CardScreen child(screen.frame(), screen.theme());
  const auto all = child.contentRect();
  const fui::Rect inner{static_cast<int16_t>(rect.x + kCardPad), static_cast<int16_t>(rect.y + kCardPad),
                        static_cast<int16_t>(rect.width - 2 * kCardPad), static_cast<int16_t>(rect.height - 2 * kCardPad)};
  child.insetContent(fui::Insets{static_cast<int16_t>(inner.y - all.y), static_cast<int16_t>(all.right() - inner.right()),
                                 static_cast<int16_t>(all.bottom() - inner.bottom()), static_cast<int16_t>(inner.x - all.x)});
  return child;
}

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const bool overReader)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer, true) {
  this->overReader = overReader;
}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  volume = std::min<uint8_t>(SETTINGS.speakerVolume, 100);
  HalAudioOutput::setVolume(volume);
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;
  lastSliderRenderAt = 0;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  resetUi();
  // The host app holds six handlers; light-capable boards use all six.
  if (!Frontlight.present()) {
    app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
    app.setScreen(&FrontlightPanelActivity::panelScreen, this);
    requestUpdate();
    return;
  }
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.speakerVolume != volume || SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.speakerVolume = volume;
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  const auto next = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  if (next == self->brightness && self->lightOn) return;
  self->brightness = next;
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  const auto next = percentFromPermille(event.dragPermille);
  if (next == self->warmth) return;
  self->warmth = next;
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille >= 0 || event.value == kVolumeStep || event.value == -kVolumeStep) {
    self->volume = event.dragPermille >= 0 ? percentFromPermille(event.dragPermille)
        : static_cast<uint8_t>(std::clamp<int>(self->volume + (event.value > 0 ? 1 : -1), 0, 100));
    HalAudioOutput::setVolume(self->volume);
  } else if (event.value == kVolumePreview) {
    HalAudioOutput::preview(self->volume);
  } else {
    self->runTile(event.value);
  }
}

void FrontlightPanelActivity::runTile(const int idx) {
  runRickyTile(static_cast<Tile>(idx));
  return;
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated() && (!touch.snap.touchHeld || millis() - lastSliderRenderAt >= 250)) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) draggingSlider = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::NavPrevious},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::NavNext},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  return rickyPanelHeight();
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::sectionHead(UiScreen& screen, const char* name, const int control, const int value) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const auto row = screen.takeTop(kHeadHeight, 0);
  auto label = theme.bodyText;
  label.maxLines = 1;
  const int16_t labelHeight = target.lineHeight(label.font);
  const int16_t controlWidth = control == kHeadNone ? 0 : kTouch;
  target.text(fui::Rect{row.x, static_cast<int16_t>(row.y + (row.height - labelHeight) / 2),
                        static_cast<int16_t>(row.width - controlWidth - theme.spaceMd), labelHeight},
              name, label);
  if (value >= 0) {
    // A slider without its own caption shows its level here, left of the control.
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", value);
    auto level = theme.smallText;
    level.maxLines = 1;
    level.align = fui::TextAlign::Right;
    const int16_t levelHeight = target.lineHeight(level.font);
    target.text(fui::Rect{row.x, static_cast<int16_t>(row.y + (row.height - levelHeight) / 2),
                          static_cast<int16_t>(row.width - controlWidth - theme.spaceLg), levelHeight},
                percent, level);
  }
  if (control == kHeadSwitch) {
    // The light switch: a pill with a round knob, ink when on. Its touch box is the
    // row's right touch square, clear of the [+] in the band below.
    fui::ToggleProps toggle;
    toggle.checked = lightOn;
    toggle.action = ACTION_TOGGLE;
    toggle.inputMask = fui::InputTouch;
    toggle.width = kSwitchWidth;
    toggle.height = kSwitchHeight;
    toggle.radius = kSwitchHeight / 2;
    toggle.knobRadius = (kSwitchHeight - 8) / 2;
    toggle.knobInset = 4;
    toggle.borderWidth = ricky::ds::stroke::kHairline;
    toggle.minTouchSize = kTouch;
    fui::toggle(screen.frame(),
                fui::Rect{static_cast<int16_t>(row.right() - kSwitchWidth),
                          static_cast<int16_t>(row.y + (row.height - kSwitchHeight) / 2), kSwitchWidth, kSwitchHeight},
                toggle);
  } else if (control == kHeadPreview) {
    // The sound test: a round outline button with a play mark, ink while pressed.
    const fui::Rect hit{static_cast<int16_t>(row.right() - kTouch), static_cast<int16_t>(row.y + (row.height - kTouch) / 2),
                        kTouch, kTouch};
    const fui::Rect disc{static_cast<int16_t>(hit.right() - kPreviewSize), static_cast<int16_t>(hit.y + (kTouch - kPreviewSize) / 2),
                         kPreviewSize, kPreviewSize};
    const bool pressed = fui::hasState(screen.frame().stateFor(ACTION_TILE, kVolumePreview), fui::StateActive);
    target.fill(disc, fui::Paint::solid(pressed ? fui::Color::Black : fui::Color::White), kPreviewSize / 2);
    if (!pressed) target.stroke(disc, fui::Paint::solid(fui::Color::Black), ricky::ds::stroke::kHairline, kPreviewSize / 2);
    target.bitmap(fui::Rect{static_cast<int16_t>(disc.x + (kPreviewSize - 32) / 2 + 2),
                            static_cast<int16_t>(disc.y + (kPreviewSize - 32) / 2), 32, 32},
                  fui::bitmapFromIcon(kPlayIcon), fui::BitmapMode::Center,
                  fui::Paint::solid(pressed ? fui::Color::White : fui::Color::Black));
    screen.frame().hit(hit, ACTION_TILE, kVolumePreview, fui::InputTouch);
  }
}

void FrontlightPanelActivity::pillSlider(UiScreen& screen, const fui::Rect band, const freeink::Icon& icon,
                                         const char* name, const uint8_t value, const fui::ActionId action,
                                         const int16_t actionValue) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const auto ink = fui::Paint::solid(fui::Color::Black);
  const auto paper = fui::Paint::solid(fui::Color::White);
  constexpr uint8_t radius = kPillHeight / 2;
  const fui::Rect pill{band.x, static_cast<int16_t>(band.y + (band.height - kPillHeight) / 2), band.width, kPillHeight};
  // The ink fills to the level, never narrower than the icon's disc, so 1% still reads.
  const int16_t filled = static_cast<int16_t>(
      std::max<int>(kPillHeight, pill.width * std::min<int>(value, 100) / 100));
  target.fill(pill, paper, radius);
  target.fill(fui::Rect{pill.x, pill.y, filled, kPillHeight}, ink, radius);
  target.stroke(pill, ink, ricky::ds::stroke::kHairline, radius);
  target.bitmap(fui::Rect{static_cast<int16_t>(pill.x + (kPillHeight - icon.w) / 2),
                          static_cast<int16_t>(pill.y + (kPillHeight - icon.h) / 2), static_cast<int16_t>(icon.w),
                          static_cast<int16_t>(icon.h)},
                fui::bitmapFromIcon(icon), fui::BitmapMode::Center, paper);
  // "name NN%" at the right end, in paper once the ink has reached it.
  char text[40];
  snprintf(text, sizeof(text), "%s %u%%", name ? name : "", static_cast<unsigned>(value));
  auto label = theme.bodyText;
  label.maxLines = 1;
  label.align = fui::TextAlign::Right;
  const int16_t labelHeight = target.lineHeight(label.font);
  const fui::Rect textRect{static_cast<int16_t>(pill.x + kPillHeight), static_cast<int16_t>(pill.y + (kPillHeight - labelHeight) / 2),
                           static_cast<int16_t>(pill.width - kPillHeight - radius), labelHeight};
  const int textWidth = target.measureText(label.font, text, label).width;
  label.color = filled > textRect.right() - textWidth / 2 ? fui::Color::White : fui::Color::Black;
  target.text(textRect, text, label);
  screen.frame().hit(band, action, actionValue, fui::InputTouch | fui::InputDrag);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  buildRickyPanel(screen);
  return;
}

namespace {
// Actions: one rounded tile per column holding its icon and label, inverted when on.
constexpr int16_t kTilePad = 16;
constexpr int16_t kTileLabelGap = 8;
constexpr int16_t kTileSpacing = 12;
constexpr uint8_t kTileRadius = 20;
constexpr int kActionColumns = 4;
// Clean-refresh choices in REFRESH_FREQUENCY order (pages between clean refreshes; 0 = never).
constexpr int kRefreshPages[] = {1, 5, 10, 15, 30, 0};
}  // namespace

void FrontlightPanelActivity::runRickyTile(const Tile tile) {
  app.clearTapFlash();
  switch (tile) {
    case Tile::Night:
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case Tile::Refresh:
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case Tile::Standby:
      // Standby goes on top of the panel, so waking returns to whatever the panel
      // opened over (a book stays open); the panel then closes itself.
      startActivityForResultWith<StandbyActivity>([this](const ActivityResult&) { close(); });
      break;
    case Tile::Transfer:
      activityManager.goToFileTransfer();
      break;
    case Tile::Orientation:
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case Tile::Clean:
      cycleCleanRefresh();
      break;
    case Tile::Light:
      toggleLight();
      break;
    case Tile::FastRefresh:
      SETTINGS.readerFastRefresh = SETTINGS.readerFastRefresh ? 0 : 1;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case Tile::Preview:
      break;  // ACTION_TILE dispatcher owns the audio preview command.
  }
}

void FrontlightPanelActivity::cycleCleanRefresh() {
  const int count = static_cast<int>(std::size(kRefreshPages));
  SETTINGS.refreshFrequency = static_cast<uint8_t>((std::min<int>(SETTINGS.refreshFrequency, count - 1) + 1) % count);
  SETTINGS.saveToFile();
  requestUpdate();
}

// Mirrors buildRickyPanel() band by band so the sheet is sized before it is drawn.
int FrontlightPanelActivity::rickyPanelHeight() const {
  const auto tokens = uiThemeTokens(uiTarget, true);
  const auto& metrics = uiThemeMetrics(true);
  const int small = uiTarget.lineHeight(tokens.smallText.font);
  const int title = uiTarget.lineHeight(tokens.titleText.font);
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const int tileRow = kTilePad + 40 + kTileLabelGap + small + kTilePad;
  int y = tokens.spaceMd + std::max<int>(metrics.batteryHeight, title) + tokens.spaceXs + small;  // clock, date
  if (mappedInput.hasTouch()) {
    y += kGroupGap + tileRow;
    if (!landscape && overReader) y += kTileSpacing + tileRow;
    const bool light = Frontlight.present();
    const bool warm = light && Frontlight.hasColorTemperature();
    const bool sound = HalAudioOutput::present();
    if (landscape) {
      if (light || sound) y += kGroupGap + 2 * kCardPad + kHeadHeight + kBareBandGap + kTouch;
    } else {
      if (light) y += kGroupGap + 2 * kCardPad + kHeadHeight + kBareBandGap + kTouch + (warm ? kBandGap + kTouch : 0);
      if (sound) y += kGroupGap + 2 * kCardPad + kTouch;
    }
  }
  y += tokens.spaceLg + kGrabberHeight + tokens.spaceLg + tokens.spaceMd;  // grabber band
  return y;
}

void FrontlightPanelActivity::roundActionRow(UiScreen& screen, const RoundAction* actions, const int count) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  auto label = theme.smallText;
  label.maxLines = 1;
  label.align = fui::TextAlign::Center;
  const int16_t labelHeight = target.lineHeight(theme.smallText.font);
  const auto row = screen.takeTop(static_cast<int16_t>(kTilePad + 40 + kTileLabelGap + labelHeight + kTilePad), 0);
  const int columns = std::max(kActionColumns, count);
  const int16_t width = static_cast<int16_t>((row.width - (columns - 1) * kTileSpacing) / columns);
  for (int i = 0; i < count; ++i) {
    const RoundAction& action = actions[i];
    const fui::Rect tile{static_cast<int16_t>(row.x + i * (width + kTileSpacing)), row.y, width, row.height};
    // On (or pressed) = an ink tile with icon and label in paper; off = a quiet outline.
    const bool on = action.on ||
                    fui::hasState(screen.frame().stateFor(ACTION_TILE, static_cast<int16_t>(action.tile)), fui::StateActive);
    target.fill(tile, fui::Paint::solid(on ? fui::Color::Black : fui::Color::White), kTileRadius);
    if (!on) target.stroke(tile, fui::Paint::solid(fui::Color::DarkGray), ricky::ds::stroke::kHairline, kTileRadius);
    const freeink::Icon& icon = *action.icon;
    target.bitmap(fui::Rect{static_cast<int16_t>(tile.x + (width - icon.w) / 2),
                            static_cast<int16_t>(tile.y + kTilePad + 20 - icon.opticalCenterY),
                            static_cast<int16_t>(icon.w), static_cast<int16_t>(icon.h)},
                  fui::bitmapFromIcon(icon), fui::BitmapMode::Center,
                  fui::Paint::solid(on ? fui::Color::White : fui::Color::Black));
    label.color = on ? fui::Color::White : fui::Color::Black;
    target.text(fui::Rect{static_cast<int16_t>(tile.x + 4), static_cast<int16_t>(tile.y + kTilePad + 40 + kTileLabelGap),
                          static_cast<int16_t>(width - 8), labelHeight},
                action.label, label);
    screen.frame().hit(tile, ACTION_TILE, static_cast<int16_t>(action.tile), fui::InputTouch);
  }
}

void FrontlightPanelActivity::buildRickyPanel(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::SheetProps sheetProps;
  sheetProps.grabberMargin = theme.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, theme.spaceLg, 0, theme.spaceLg});
  auto& target = screen.target();
  RickyPageUi::Bold bold(renderer, 1);
  auto small = theme.smallText;
  small.maxLines = 1;
  const int16_t smallHeight = target.lineHeight(theme.smallText.font);
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();

  // Clock on the left of the status band, battery on the right where Home draws it.
  const auto& metrics = uiThemeMetrics(true);
  screen.spacer(theme.spaceMd);
  const auto band = screen.takeTop(
      std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight), target.lineHeight(theme.titleText.font)),
      theme.spaceXs);
  GUI.drawHeaderWithStyle(
      renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding - metrics.topPadding},
      nullptr, nullptr, true, true);
  std::tm local{};
  const bool haveTime = halClock.localTime(local);
  if (!haveTime || !TimeUtils::formatCurrentTime(timeText, sizeof(timeText), SETTINGS.clockFormat == 1)) {
    snprintf(timeText, sizeof(timeText), "--:--");
  }
  auto clock = theme.titleText;
  clock.bold = true;
  clock.maxLines = 1;
  target.text(fui::Rect{band.x, band.y, static_cast<int16_t>(band.width / 2), band.height}, timeText, clock);
  char date[40] = {};
  if (haveTime) {
    static constexpr StrId weekdays[] = {
        StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE, StrId::STR_CAL_WEEKDAY_WED,
        StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI, StrId::STR_CAL_WEEKDAY_SAT};
    snprintf(date, sizeof(date), tr(STR_RICKY_HOME_DATE), static_cast<unsigned>(local.tm_mon + 1),
             static_cast<unsigned>(local.tm_mday), I18N.get(weekdays[std::clamp(local.tm_wday, 0, 6)]));
  }
  char today[40];
  snprintf(today, sizeof(today), tr(STR_RICKY_HOME_TODAY),
           static_cast<unsigned>(READING_STATS.getTodayReadingMs() / 60000));
  snprintf(infoText, sizeof(infoText), haveTime ? "%s · %s" : "%s%s", date, today);
  target.text(screen.takeTop(smallHeight, 0), infoText, small);

  // Every control below is a touch target; a buttons-only board keeps just the clock.
  if (!mappedInput.hasTouch()) return;
  screen.spacer(kGroupGap);
  RoundAction actions[7] = {
      {Tile::Night, &icon_ricky_night_40, tr(STR_NIGHT_MODE), SETTINGS.screenInverted != 0},
      {Tile::Refresh, &icon_ricky_refresh_40, tr(STR_FORCE_REFRESH), false},
      {Tile::Standby, &icon_ricky_standby_40, tr(STR_RICKY_STANDBY_NOW), false},
      {Tile::Transfer, &icon_ricky_transfer_40, tr(STR_FILE_TRANSFER), false},
  };
  static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                            StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
  const uint8_t orientation = SETTINGS.orientation % 4;
  const int refreshIndex = std::min<int>(SETTINGS.refreshFrequency, static_cast<int>(std::size(kRefreshPages)) - 1);
  if (kRefreshPages[refreshIndex] == 0) {
    snprintf(refreshText, sizeof(refreshText), "%s", tr(STR_RICKY_CC_FULL_REFRESH_OFF));
  } else {
    snprintf(refreshText, sizeof(refreshText), tr(STR_RICKY_CC_FULL_REFRESH), kRefreshPages[refreshIndex]);
  }
  const RoundAction reading[] = {
      {Tile::Orientation, orientation % 2 ? &icon_ricky_landscape_40 : &icon_ricky_portrait_40,
       I18N.get(kOrientNames[orientation]), false},
      {Tile::Clean, &icon_ricky_clean_40, refreshText, false},
      // Reading refresh: 快刷 (on) or 高清.
      {Tile::FastRefresh, &icon_ricky_fast_40, tr(STR_RICKY_CC_FAST_REFRESH), SETTINGS.readerFastRefresh != 0},
  };
  int count = 4;
  // Landscape has room for the reading actions in the same row.
  if (landscape && overReader)
    for (const auto& action : reading) actions[count++] = action;
  roundActionRow(screen, actions, count);
  // Reading: only over a book, where each of these changes the page underneath.
  if (!landscape && overReader) {
    screen.spacer(kTileSpacing);
    roundActionRow(screen, reading, static_cast<int>(std::size(reading)));
  }

  const bool light = Frontlight.present();
  const bool warm = light && Frontlight.hasColorTemperature();
  const bool sound = HalAudioOutput::present();
  if (!landscape) {
    if (light) {
      screen.spacer(kGroupGap);
      auto card = cardArea(screen, static_cast<int16_t>(2 * kCardPad + kHeadHeight + kBareBandGap + kTouch +
                                                         (warm ? kBandGap + kTouch : 0)));
      sectionHead(card, tr(STR_FRONTLIGHT), kHeadSwitch);
      card.spacer(kBareBandGap);
      pillSlider(card, card.takeTop(kTouch, kBandGap), icon_sun_32, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, 0);
      if (warm) pillSlider(card, card.takeTop(kTouch, 0), icon_moon_32, tr(STR_RICKY_CC_WARMTH), warmth, ACTION_WARMTH, 0);
    }
    if (sound) {
      screen.spacer(kGroupGap);
      auto card = cardArea(screen, static_cast<int16_t>(2 * kCardPad + kTouch));
      auto band = card.takeTop(kTouch, 0);
      // The sound test sits at the right end of the volume pill's band.
      const fui::Rect hit{static_cast<int16_t>(band.right() - kTouch), band.y, kTouch, kTouch};
      band.width = static_cast<int16_t>(band.width - kTouch - theme.spaceMd);
      pillSlider(card, band, kSpeakerIcon, tr(STR_RICKY_CC_VOLUME), volume, ACTION_TILE, kVolumeDrag);
      const fui::Rect disc{static_cast<int16_t>(hit.x + (kTouch - kPillHeight) / 2), static_cast<int16_t>(hit.y + (kTouch - kPillHeight) / 2),
                           kPillHeight, kPillHeight};
      const bool pressed = fui::hasState(screen.frame().stateFor(ACTION_TILE, kVolumePreview), fui::StateActive);
      target.fill(disc, fui::Paint::solid(pressed ? fui::Color::Black : fui::Color::White), kPillHeight / 2);
      if (!pressed) target.stroke(disc, fui::Paint::solid(fui::Color::Black), ricky::ds::stroke::kHairline, kPillHeight / 2);
      target.bitmap(fui::Rect{static_cast<int16_t>(disc.x + (kPillHeight - 32) / 2 + 2),
                              static_cast<int16_t>(disc.y + (kPillHeight - 32) / 2), 32, 32},
                    fui::bitmapFromIcon(kPlayIcon), fui::BitmapMode::Center,
                    fui::Paint::solid(pressed ? fui::Color::White : fui::Color::Black));
      screen.frame().hit(hit, ACTION_TILE, kVolumePreview, fui::InputTouch);
    }
    return;
  }
  // Landscape: one card per slider side by side, each a head over its pill.
  const int columns = (light ? 1 : 0) + (warm ? 1 : 0) + (sound ? 1 : 0);
  if (!columns) return;
  screen.spacer(kGroupGap);
  const int16_t height = static_cast<int16_t>(2 * kCardPad + kHeadHeight + kBareBandGap + kTouch);
  const auto area = screen.takeTop(height, 0);
  constexpr int16_t kGap = 20;
  const int16_t width = static_cast<int16_t>((area.width - (columns - 1) * kGap) / columns);
  int column = 0;
  const auto next = [&]() {
    UiScreen child(screen.frame(), theme);
    const auto all = child.contentRect();
    const int16_t x = static_cast<int16_t>(area.x + column++ * (width + kGap));
    child.insetContent(fui::Insets{static_cast<int16_t>(area.y - all.y), static_cast<int16_t>(all.right() - x - width),
                                   static_cast<int16_t>(all.bottom() - area.bottom()), static_cast<int16_t>(x - all.x)});
    return cardArea(child, height);
  };
  if (light) {
    auto card = next();
    sectionHead(card, tr(STR_FRONTLIGHT), kHeadSwitch);
    card.spacer(kBareBandGap);
    pillSlider(card, card.takeTop(kTouch, 0), icon_sun_32, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, 0);
  }
  if (warm) {
    auto card = next();
    sectionHead(card, tr(STR_RICKY_CC_WARMTH), kHeadNone);
    card.spacer(kBareBandGap);
    pillSlider(card, card.takeTop(kTouch, 0), icon_moon_32, tr(STR_RICKY_CC_WARMTH), warmth, ACTION_WARMTH, 0);
  }
  if (sound) {
    auto card = next();
    sectionHead(card, tr(STR_RICKY_CC_VOLUME), kHeadPreview);
    card.spacer(kBareBandGap);
    pillSlider(card, card.takeTop(kTouch, 0), kSpeakerIcon, tr(STR_RICKY_CC_VOLUME), volume, ACTION_TILE, kVolumeDrag);
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); the battery band at the card's top is part of the screen build.
  renderUi();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
  lastSliderRenderAt = millis();
}
