"""Exercise actual control-center light callbacks and layout against a fake HAL."""
from pathlib import Path
import subprocess
import tempfile

source = Path('firmware/rickyos-plus-1.2/upstream/src/activities/util/FrontlightPanelActivity.cpp').read_text()
signatures = [
    'uint8_t percentFromPermille(',
    'void FrontlightPanelActivity::onBrightnessEvent(',
    'void FrontlightPanelActivity::onWarmthEvent(',
    'void FrontlightPanelActivity::onTileEvent(',
    'void FrontlightPanelActivity::adjustBrightness(',
    'void FrontlightPanelActivity::adjustWarmth(',
    'void FrontlightPanelActivity::toggleLight(',
    'void FrontlightPanelActivity::persistLightSettings(',
    'int FrontlightPanelActivity::rickyPanelHeight() const',
]
functions = []
for signature in signatures:
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    functions.append(source[start:cursor])

program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
constexpr uint8_t MIN_BRIGHTNESS = 1;
constexpr int kVolumeStep=101, kVolumePreview=200;
constexpr int kTilePad=16, kTileLabelGap=8, kTileSpacing=12, kGroupGap=20, kCardPad=16, kBandGap=8, kBareBandGap=20, kTouch=88, kHeadHeight=64, kGrabberHeight=5;
namespace HalAudioOutput {
 bool available=false; int current=0, previews=0;
 bool present() { return available; }
 void setVolume(int v) { current=v; }
 bool preview(int) { ++previews; return true; }
}
namespace fui { struct ActionEvent { int16_t dragPermille=-1; int value=0; }; }
struct Light {
 bool lit=false, available=true, dual=true;
 uint8_t brightness=60, warmth=50;
 int writes=0;
 void setBrightness(uint8_t v) { brightness=v; ++writes; }
 void setWarmth(uint8_t v) { warmth=v; ++writes; }
 void setOn(bool v) { lit=v; ++writes; }
 bool present() const { return available; }
 bool hasColorTemperature() const { return dual; }
} Frontlight;
struct Settings {
 uint8_t speakerVolume=35;
 uint8_t frontlightBrightness=60, frontlightWarmth=50, frontlightOn=1;
 int saves=0;
 void saveToFile() { ++saves; }
} SETTINGS;
struct Target {
 int lineHeight(int font) const { return font==1 ? 28 : 44; }
};
struct Tokens {
 int spaceMd=16, spaceXs=2, spaceLg=24;  // the RickyOS high-DPI theme (UIThemeTokens.cpp)
 struct Text { int font; } smallText{1}, titleText{2};
};
Tokens uiThemeTokens(const Target&, bool) { return {}; }
struct Metrics { int batteryHeight=20; };
Metrics uiThemeMetrics(bool) { return {}; }
struct Renderer {
 int width=684, height=1216;
 int getScreenWidth() const { return width; }
 int getScreenHeight() const { return height; }
};
struct Input { bool touch=true; bool hasTouch() const { return touch; } };
class FrontlightPanelActivity {
public:
 uint8_t volume=35;
 uint8_t brightness=60, warmth=50;
 bool lightOn=false, lightOnChanged=false, overReader=false;
 int updates=0;
 struct App {void clearTapFlash(){}} app;void close() {}
 Target uiTarget; Renderer renderer; Input mappedInput;
 void requestUpdate() { ++updates; }
 static void onBrightnessEvent(const fui::ActionEvent&,void*);
 static void onWarmthEvent(const fui::ActionEvent&,void*);
 static void onTileEvent(const fui::ActionEvent&,void*);
 void runTile(int) {}
 void adjustBrightness(int); void adjustWarmth(int); void toggleLight();
 void persistLightSettings(); int rickyPanelHeight() const;
};
FUNCTIONS
int main() {
 FrontlightPanelActivity audioPanel;
 audioPanel.onTileEvent({1000,0},&audioPanel); assert(audioPanel.volume==100 && HalAudioOutput::current==100);
 audioPanel.onTileEvent({-1,101},&audioPanel); assert(audioPanel.volume==100);
 audioPanel.onTileEvent({0,0},&audioPanel); assert(audioPanel.volume==0 && HalAudioOutput::current==0);
 audioPanel.onTileEvent({-1,-101},&audioPanel); assert(audioPanel.volume==0);
 audioPanel.onTileEvent({-1,200},&audioPanel); assert(HalAudioOutput::previews==1);
 HalAudioOutput::available=true;
 audioPanel.overReader=true; assert(audioPanel.rickyPanelHeight()<=1216);
 audioPanel.renderer.width=1216; audioPanel.renderer.height=684; assert(audioPanel.rickyPanelHeight()<=684);
 HalAudioOutput::available=false;
 FrontlightPanelActivity p;
 // Merely visiting after wake-with-light-off must preserve saved On preference.
 p.persistLightSettings(); assert(SETTINGS.saves==0 && SETTINGS.frontlightOn==1);
 p.onBrightnessEvent({600},&p); assert(p.lightOn && Frontlight.lit);
 p.onBrightnessEvent({0},&p); assert(p.brightness==1 && Frontlight.brightness==1);
 p.onBrightnessEvent({1000},&p); assert(p.brightness==100);
 int writes=Frontlight.writes; p.onBrightnessEvent({1000},&p); assert(Frontlight.writes==writes);
 p.onBrightnessEvent({-1},&p); assert(Frontlight.writes==writes);
 p.toggleLight(); assert(!p.lightOn && p.brightness==100);
 p.onWarmthEvent({1000},&p); assert(p.warmth==100 && !p.lightOn && !Frontlight.lit);
 p.onWarmthEvent({0},&p); assert(p.warmth==0);
 p.adjustWarmth(-1); assert(p.warmth==0);
 p.adjustWarmth(101); assert(p.warmth==100);
 p.toggleLight(); assert(p.lightOn && p.brightness==100 && p.warmth==100);
 p.persistLightSettings(); assert(SETTINGS.saves==1 && SETTINGS.frontlightBrightness==100);
 p.persistLightSettings(); assert(SETTINGS.saves==1);
 p.adjustBrightness(-200); assert(p.brightness==1);
 p.adjustBrightness(200); assert(p.brightness==100);
 p.toggleLight(); p.persistLightSettings(); assert(SETTINGS.frontlightOn==0);
 for (bool dual : {false,true}) for (bool touch : {false,true}) for(bool reader : {false,true}) {
   Frontlight.dual=dual; p.mappedInput.touch=touch; p.overReader=reader;
   p.renderer={684,1216}; int portrait=p.rickyPanelHeight(); assert(portrait<=1216);
   p.renderer={1216,684}; int landscape=p.rickyPanelHeight(); assert(landscape<=684);
   // Without touch the sheet is only the clock, as on the Pico; with it landscape packs into columns.
   if(touch && dual) assert(landscape<portrait);
   if(!touch) assert(landscape==portrait);
 }
 Frontlight.available=false; p.renderer={684,1216};
 int withoutLight=p.rickyPanelHeight(); Frontlight.available=true;
 assert(p.rickyPanelHeight()>withoutLight);
 const int saved=SETTINGS.saves;
 p.onTileEvent({700,0},&p); assert(p.volume==70 && SETTINGS.saves==saved);
 p.persistLightSettings(); assert(SETTINGS.speakerVolume==70 && SETTINGS.saves==saved+1);
 p.persistLightSettings(); assert(SETTINGS.saves==saved+1);
}
'''.replace('FUNCTIONS', '\n'.join(functions))
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root/'panel.cpp').write_text(program)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(root/'panel.cpp'),'-o',str(root/'panel')],check=True)
    subprocess.run([str(root/'panel')],check=True)
print('Actual light callbacks: clamps, no duplicate writes, off-state memory, save-on-change and portrait/landscape layout passed')
