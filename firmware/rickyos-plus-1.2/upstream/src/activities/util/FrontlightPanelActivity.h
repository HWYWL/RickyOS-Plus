#pragma once

#include <Icon.h>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored RickyOS control center: rectangular quick actions, grouped
// frontlight and audio controls, visible Close and swipe dismissal. Existing
// FUI routing, live HAL values and save-on-exit behavior are retained.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t volume = 35;
  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // legitimately diverges from the saved SETTINGS.frontlightOn preference —
  // e.g. after a wake with frontlightRestoreOnWake off, the light stays off
  // live while the saved "was on" preference is deliberately kept (see
  // main.cpp's restoreLightOn). brightness/warmth have no such divergence
  // (always restored unconditionally on boot), so only lightOn needs a
  // touched-by-the-user flag: onExit() must not persist a mirror that never
  // reflected user intent in the first place.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  uint32_t lastSliderRenderAt = 0;
  // The touch tile toggles SETTINGS.touchReaderControls between off and this
  // remembered mode, so a Swipe or Inverted Tap user gets their mode back
  // rather than the Tap default. Seeded from the setting in onEnter().
  uint8_t touchModeRestore = CrossPointSettings::TOUCH_READER_ON;
  int panelBottom = 0;

  // RickyOS: the Pico's sheet (clock header, one row of actions, a reading row only
  // over a book) plus the Plus front light (switch, brightness, warmth) and volume.
  enum class Tile : uint8_t { Night, Refresh, Standby, Transfer, Orientation, Clean, Light, FastRefresh, Preview = 200 };
  struct RoundAction {
    Tile tile;
    const freeink::Icon* icon;
    const char* label;
    bool on;
  };
  bool overReader = false;
  char refreshText[24] = {};
  void runRickyTile(Tile tile);
  void cycleCleanRefresh();
  int rickyPanelHeight() const;
  void buildRickyPanel(UiScreen& screen);
  void roundActionRow(UiScreen& screen, const RoundAction* actions, int count);

  char timeText[12] = {};
  char infoText[80] = {};

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // A section head: the name on the left, an optional control (the light switch, the
  // sound test) on the right, and the level (value >= 0) beside the control.
  void sectionHead(UiScreen& screen, const char* name, int control, int value = -1);
  // One slider: a thick pill filled in ink up to the level, its icon at the left and
  // "name NN%" at the right. The whole touch band is the drag target.
  void pillSlider(UiScreen& screen, freeink::ui::Rect band, const freeink::Icon& icon, const char* name,
                  uint8_t value, freeink::ui::ActionId action, int16_t actionValue);
  int computePanelBottom() const;
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void runTile(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on every way out.
  void persistLightSettings();
  void close();

  // One-shot: a tile that rewrote the whole frame (night mode) re-drives it
  // with the ghost-cleanup waveform on the next render. The "refresh" tile does
  // not use this — it closes the panel and promotes the repaint underneath
  // instead (GfxRenderer::promoteNextRefresh).
  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool overReader = false);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
