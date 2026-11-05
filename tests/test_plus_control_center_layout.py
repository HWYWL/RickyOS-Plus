"""Render the real control-center methods with real FUI and audit every hit rectangle."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
u = ROOT / 'firmware/rickyos-plus-1.2/upstream'
s = (u / 'src/activities/util/FrontlightPanelActivity.cpp').read_text()


def block(start, end_marker='}  // namespace'):
    a = s.index(start)
    return s[a:s.index(end_marker, a) + len(end_marker)]


def method(signature):
    a = s.index(signature)
    b = s.index('{', a) + 1
    depth = 1
    while depth:
        depth += (s[b] == '{') - (s[b] == '}')
        b += 1
    return s[a:b]


constants = block('namespace {\nconstexpr fui::ActionId ACTION_BRIGHTNESS') + '\n' + block('namespace {\n// Actions:')
methods = [method(sig) for sig in (
    'void FrontlightPanelActivity::sectionHead(',
    'void FrontlightPanelActivity::pillSlider(',
    'int FrontlightPanelActivity::rickyPanelHeight() const',
    'void FrontlightPanelActivity::roundActionRow(',
    'void FrontlightPanelActivity::buildRickyPanel(',
)]
ids = sorted(set(re.findall(r'(?:StrId::)?(STR_[A-Z0-9_]+)', ''.join(methods))))

code = r'''
#include <FreeInkApp.h>
#include <FreeInkUIIcon.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
namespace fui = freeink::ui;
using UiScreen = fui::Screen<24>;
struct UiAppHost { using UiScreen = ::UiScreen; };
enum class StrId { @IDS@ };
@IDCONSTS@
const char* tr(StrId) { return "test"; }
struct { const char* get(StrId) { return "test"; } } I18N;
namespace ricky::ds {
namespace layout { constexpr int16_t kTouchMin = 88, kAppTileSize = 112; }
namespace radius { constexpr uint8_t kControl = 12, kContainer = 24; }
namespace stroke { constexpr uint8_t kHairline = 2; }
}
struct { bool available = true; } audio;
namespace HalAudioOutput { bool present() { return audio.available; } }
struct { bool available = true, dual = true; bool present() const { return available; }
         bool hasColorTemperature() const { return dual; } } Frontlight;
struct { int orientation = 0, refreshFrequency = 0, screenInverted = 0, clockFormat = 0, readerFastRefresh = 0; } SETTINGS;
struct { bool localTime(std::tm& t) const { t = {}; t.tm_mon = 9; t.tm_mday = 14; return true; } } halClock;
namespace TimeUtils { bool formatCurrentTime(char* out, size_t size, bool) { snprintf(out, size, "12:00"); return true; } }
struct { unsigned long long getTodayReadingMs() const { return 0; } } READING_STATS;
struct Target : fui::DrawTarget {
  fui::Size measureText(fui::FontId f, const char* t, fui::TextStyle) const override {
    return {static_cast<int16_t>(strlen(t) * 10), lineHeight(f)};
  }
  int16_t lineHeight(fui::FontId f) const override { return f == 2 ? 44 : f == 3 ? 34 : 28; }
  void fill(fui::Rect, fui::Paint, uint8_t = 0, uint8_t = fui::CornersAll) override {}
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t = 0, uint8_t = fui::CornersAll) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect, const char*, fui::TextStyle) override {}
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint = fui::Paint::solid(fui::Color::Black),
              fui::Rotation = fui::Rotation::None) override {}
};
fui::ThemeTokens tokens() {
  fui::ThemeTokens t;
  t.smallText.font = 1;
  t.titleText.font = 2;
  t.bodyText.font = 3;
  t.minTouchSize = 88;
  t.sheetRadius = 24;
  t.spaceXs = 2; t.spaceSm = 8; t.spaceMd = 16; t.spaceLg = 24;  // UIThemeTokens.cpp high-DPI values
  return t;
}
fui::ThemeTokens uiThemeTokens(const Target&, bool) { return tokens(); }
struct { int topPadding = 5, homeTopPadding = 48, batteryHeight = 28; } metrics;
auto& uiThemeMetrics(bool) { return metrics; }
struct Renderer { int width = 684, height = 1216; int getScreenWidth() const { return width; }
                  int getScreenHeight() const { return height; } };
struct Rect { int x, y, width, height; };
struct { void drawHeaderWithStyle(Renderer&, Rect, void*, void*, bool, bool) {} } GUI;
namespace RickyPageUi { struct Bold { Bold(Renderer&, int) {} }; }
uint8_t iconBits[200]{};
const freeink::Icon icon_ricky_night_40{40, 40, 20, iconBits}, icon_ricky_refresh_40{40, 40, 20, iconBits},
    icon_ricky_standby_40{40, 40, 20, iconBits}, icon_ricky_transfer_40{40, 40, 20, iconBits},
    icon_ricky_portrait_40{40, 40, 20, iconBits}, icon_ricky_landscape_40{40, 40, 20, iconBits},
    icon_ricky_clean_40{40, 40, 20, iconBits}, icon_ricky_fast_40{40, 40, 20, iconBits}, icon_sun_32{32, 32, 16, iconBits}, icon_moon_32{32, 32, 16, iconBits};
@CONSTANTS@
class FrontlightPanelActivity {
 public:
  enum class Tile : uint8_t { Night, Refresh, Standby, Transfer, Orientation, Clean, Light, FastRefresh, Preview = 200 };
  struct RoundAction { Tile tile; const freeink::Icon* icon; const char* label; bool on; };
  Renderer renderer;
  Target uiTarget;
  struct { bool hasTouch() const { return true; } } mappedInput;
  uint8_t brightness = 60, warmth = 50, volume = 35;
  bool lightOn = false, overReader = false;
  int panelBottom = 0;
  char refreshText[24] = {}, timeText[12] = {}, infoText[80] = {};
  int rickyPanelHeight() const;
  void sectionHead(UiScreen&, const char*, int, int = -1);
  void pillSlider(UiScreen&, fui::Rect, const freeink::Icon&, const char*, uint8_t, fui::ActionId, int16_t);
  void roundActionRow(UiScreen&, const RoundAction*, int);
  void buildRickyPanel(UiScreen&);
};
@METHODS@
int main() {
  for (bool portrait : {true, false}) for (bool reader : {true, false}) for (bool light : {true, false})
  for (bool sound : {true, false}) for (bool dual : {true, false}) {
    Frontlight.available = light; Frontlight.dual = dual; audio.available = sound;
    FrontlightPanelActivity p;
    p.renderer = portrait ? Renderer{684, 1216} : Renderer{1216, 684};
    p.overReader = reader;
    p.lightOn = true;
    p.panelBottom = p.rickyPanelHeight();
    assert(p.panelBottom <= p.renderer.height);
    fui::DeviceContext device;
    device.width = p.renderer.width; device.height = p.renderer.height;
    device.hasTouch = true; device.safeArea = {8, 8, 8, 8};
    fui::InputSnapshot input;
    fui::InteractionBuffer<24> hits;
    fui::Frame<24> frame(p.uiTarget, device, input, hits);
    auto theme = tokens();
    UiScreen screen(frame, theme);
    p.buildRickyPanel(screen);
    assert(!hits.overflowed());
    // The measured height is exactly what was drawn plus the grabber band (the sheet's
    // content starts below the safe area, which rickyPanelHeight() does not count).
    const auto rest = screen.contentRect();
    assert(rest.y - device.safeArea.top + theme.spaceLg + 5 + theme.spaceLg + theme.spaceMd == p.panelBottom);
    int sliders = 0, switches = 0, previews = 0;
    for (size_t i = 0; i < hits.count(); ++i) {
      const auto a = hits.data()[i];
      assert(a.rect.width >= 88 && a.rect.height >= 88);
      assert(a.rect.y >= 0 && a.rect.bottom() <= p.panelBottom);
      if (a.inputMask & fui::InputDrag) ++sliders;
      if (a.action == ACTION_TOGGLE) ++switches;
      if (a.action == ACTION_TILE && a.value == kVolumePreview) ++previews;
      for (size_t j = 0; j < i; ++j) {
        const auto b = hits.data()[j];
        assert(a.rect.right() + 8 <= b.rect.x || b.rect.right() + 8 <= a.rect.x ||
               a.rect.bottom() + 8 <= b.rect.y || b.rect.bottom() + 8 <= a.rect.y);
      }
    }
    assert(sliders == (light ? (dual ? 2 : 1) : 0) + (sound ? 1 : 0));
    assert(switches == (light ? 1 : 0));
    assert(previews == (sound ? 1 : 0));
  }
}
'''
code = (code.replace('@IDS@', ','.join(ids))
            .replace('@IDCONSTS@', '\n'.join(f'constexpr auto {i} = StrId::{i};' for i in ids))
            .replace('@CONSTANTS@', constants)
            .replace('@METHODS@', '\n'.join(methods)))
with tempfile.TemporaryDirectory(prefix='plus-cc-layout-') as folder:
    p = Path(folder)
    (p / 'test.cpp').write_text(code)
    sdk = u / 'freeink-sdk/libs'
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-const-variable',
                    '-Wno-unused-function', '-Wno-comment', '-fsanitize=address,undefined',
                    '-I', str(sdk / 'ui/FreeInkUI/include'), '-I', str(sdk / 'assets/Icons/include'),
                    str(p / 'test.cpp'), str(sdk / 'ui/FreeInkUI/src/FreeInkUI.cpp'), '-o', str(p / 'test')],
                   check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Actual CC/FUI layout passed: 32 orientation/reader/light/warmth/audio combinations, sliders, switch and '
      'sound test present, measured height matches, 88px targets, 8px gaps, no overflow')
