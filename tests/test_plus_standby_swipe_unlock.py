"""Exercise actual Standby input and mapping methods, including touch Back leakage."""
from pathlib import Path
import re
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
SOURCE = PORT / 'firmware/rickyos-plus-1.2/upstream'


def method(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


mapped = (SOURCE / 'src/MappedInputManager.cpp').read_text()
standby = (SOURCE / 'src/activities/apps/standby/StandbyActivity.cpp').read_text()
settings = (SOURCE / 'src/CrossPointSettings.cpp').read_text()
header = (SOURCE / 'src/CrossPointSettings.h').read_text()
options = (SOURCE / 'src/activities/settings/RickyOptionListActivity.cpp').read_text()
default = re.search(r'uint8_t standbySwipeUnlock = \d+;', header).group()
save = re.search(r'  doc\["standbySwipeUnlock"\] = standbySwipeUnlock;', settings).group()
load = re.search(r'  standbySwipeUnlock = .*?;', settings[settings.index('bool CrossPointSettings::fromJson('):]).group()
assert '{StrId::STR_RICKY_STANDBY_SWIPE_UNLOCK, &SETTINGS.standbySwipeUnlock}' in options
for language in ('chinese', 'english'):
    assert 'STR_RICKY_STANDBY_SWIPE_UNLOCK:' in (SOURCE / f'lib/I18n/translations/{language}.yaml').read_text()

# New label uses existing glyphs. Do not silently fall back to missing-glyph boxes
# or regenerate unrelated large font tables if its complete subset is present.
for name in ('notosans_cjk_12_rickyos', 'notosans_cjk_14', 'notosans_cjk_16',
             'ricky_caption_10_regular', 'ricky_headline_14_medium', 'ricky_title_16_bold'):
    font = (SOURCE / f'lib/EpdFont/builtinFonts/{name}.h').read_text()
    intervals = re.search(r'EpdUnicodeInterval ' + name + r'Intervals\[\] = \{(.*?)\n\};', font, re.S).group(1)
    spans = [(int(a, 0), int(b, 0)) for a, b in re.findall(r'\{\s*(0x[\da-fA-F]+|\d+),\s*(0x[\da-fA-F]+|\d+),', intervals)]
    assert all(any(a <= ord(char) <= b for a, b in spans) for char in '锁屏右滑解锁'), name

program = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include "FreeInkUICore.h"
namespace fui = freeink::ui;
#define FREEINK_DEVICE_METALIO_PLUS 1
#define FREEINK_CAP_TOUCH 0
#define STANDBY_DIAG(...) ((void)0)
uint32_t now = 100;
uint32_t millis() { return now; }
struct JsonValue {
 int value=0; bool present=false;
 int operator|(int fallback)const { return present ? value : fallback; }
 JsonValue& operator=(int n){value=n;present=true;return *this;}
};
using Json = std::map<std::string,JsonValue>;
struct Settings {
''' + default + r'''
 uint8_t standbyShortcutEnabled=1, rickyGestureBack=1, rickyGestureHome=1;
 void save(Json& doc){
''' + save + r'''
 }
 void load(Json& doc){
''' + load + r'''
 }
 unsigned long getStandbyPowerOffMs()const{return 0;}
} SETTINGS;
struct HalGPIO {bool wasReleased(uint8_t)const{return false;}};
struct {
 bool tap=false;
 bool wasTouchTap(float& x,float& y){x=y=0;return tap;}
} gpio;
struct {
 int width=684,height=1216;
 void tapToLogical(float,float,int& x,int& y){x=y=0;}
 int getScreenWidth()const{return width;} int getScreenHeight()const{return height;}
} renderer;
namespace HeaderBackTapTarget {bool contains(int,int){return gpio.tap;}}
enum class HomeButtonAction {Ignore,Confirm,Back};
struct MappedInputManager {
 enum class Button {Back,Confirm}; enum class SwipeDir {None,Right,Left,Up,Down};
 uint16_t plusActions=0;
 HomeButtonAction homeAction=HomeButtonAction::Ignore;
 int bleReleaseEdges=0;
 bool physicalBack=false, physicalConfirm=false;
 bool swipe=false; int sx=0,sy=200,ex=200,ey=200;
 bool decodeSwipe(int& a,int& b,int& c,int& d)const{
  a=sx;b=sy;c=ex;d=ey;return swipe;
 }
 void rememberTouchHeldTime()const{}
 bool wasEdgeSwipe(fui::ScreenEdge)const;
 bool wasBackGesture()const;
 bool wasStandbyUnlockGesture()const;
 bool wasReleased(Button)const;
 bool mapButton(Button b,bool(HalGPIO::*)(uint8_t)const)const{
  return b==Button::Back ? physicalBack : physicalConfirm;
 }
 bool bleEdge(int,Button)const{return false;}
 bool wasScreenTapped(int&,int&)const{return false;}
 bool wasAnyReleased()const{return physicalBack||physicalConfirm;}
 SwipeDir wasSwipe()const{return swipe ? SwipeDir::Right : SwipeDir::None;}
} mappedInput;
struct StandbyFace {
 enum class PictureAction {Choose,Download};
 bool needsPicture()const{return false;}
 PictureAction pictureActionAt(int,int)const{return PictureAction::Choose;}
 void onEnter(){}
};
struct ActivityResult{}; struct RickyWallpaperDownloadActivity{};
struct {void requestPowerOff(){}} activityManager;
#define LOG_ERR(...) ((void)0)
struct StandbyActivity {
 StandbyFace* currentFace_=nullptr;
 uint32_t lastInputMs_=0,enteredMs_=0;bool drewFrame_=false;
 int finishes=0,pumps=0;
 void loop(); void finish(){++finishes;}
 void openPicturePicker(){} void requestUpdate(){}
 template<class T,class F> bool startActivityForResultWith(F){return true;}
 void pumpTimeSync(){++pumps;}
 bool tryLightSleep(uint32_t){return false;}
 void processFaceTick(bool){}
};
''' + '\n'.join(method(mapped, signature) for signature in (
    'bool MappedInputManager::wasEdgeSwipe(',
    'bool MappedInputManager::wasBackGesture()',
    'bool MappedInputManager::wasStandbyUnlockGesture()',
    'bool MappedInputManager::wasReleased(')) + '\n' + method(standby, 'void StandbyActivity::loop()') + r'''
int main(){
 // Old settings and defaults do not opt users into touch unlock.
 assert(SETTINGS.standbySwipeUnlock==0);
 Json doc;SETTINGS.standbySwipeUnlock=1;SETTINGS.load(doc);
 assert(SETTINGS.standbySwipeUnlock==0);
 for(int value : {0,1}){
  SETTINGS.standbySwipeUnlock=value;SETTINGS.save(doc);
  SETTINGS.standbySwipeUnlock=!value;SETTINGS.load(doc);
  assert(SETTINGS.standbySwipeUnlock==value);
 }
 for(int back : {0,1})for(int home : {0,1})for(int shortcut : {0,1}){
  SETTINGS.rickyGestureBack=back;SETTINGS.rickyGestureHome=home;
  SETTINGS.standbyShortcutEnabled=shortcut;
  for(int enabled : {0,1}){
   SETTINGS.standbySwipeUnlock=enabled;
   mappedInput=MappedInputManager{};mappedInput.swipe=true;
   StandbyActivity a;a.loop();assert(a.finishes==enabled);
   // Other edges, wrong direction and a central swipe are not unlock gestures.
   for(int direction : {0,1,2,3}){
    mappedInput.sx=direction==0 ? 400 : 200;
    mappedInput.sy=direction==1 ? 0 : 600;
    mappedInput.ex=direction==0 ? 0 : direction==3 ? 500 : 200;
    mappedInput.ey=direction==1 ? 500 : direction==2 ? 0 : 600;
    StandbyActivity b;b.loop();assert(b.finishes==0);
   }
   // A remembered header Back target is not a physical key.
   mappedInput=MappedInputManager{};gpio.tap=true;
   StandbyActivity tapped;tapped.loop();assert(tapped.finishes==0);gpio.tap=false;
   // Physical Back keeps its existing independent entry/exit switch.
   mappedInput.physicalBack=true;
   StandbyActivity physical;physical.loop();assert(physical.finishes==shortcut);
   mappedInput=MappedInputManager{};
   mappedInput.plusActions=1u<<static_cast<unsigned>(MappedInputManager::Button::Back);
   StandbyActivity remapped;remapped.loop();assert(remapped.finishes==shortcut);
  }
 }
 // Real SDK geometry respects portrait/landscape logical dimensions.
 for(bool landscape : {false,true}){
  renderer.width=landscape ? 1216 : 684;renderer.height=landscape ? 684 : 1216;
  mappedInput=MappedInputManager{};mappedInput.swipe=true;
  mappedInput.sx=0;mappedInput.sy=renderer.height/2;
  mappedInput.ex=renderer.width/2;mappedInput.ey=mappedInput.sy;
  assert(mappedInput.wasStandbyUnlockGesture());
 }
}
'''
with tempfile.TemporaryDirectory(prefix='plus-standby-swipe-') as temporary:
    folder = Path(temporary)
    (folder / 'test.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-I' + str(SOURCE / 'freeink-sdk/libs/ui/FreeInkUI/include'),
                    str(folder / 'test.cpp'), '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
print('Standby swipe: opt-in, JSON round-trip/missing key, global gesture independence, real SDK edge geometry, physical Back and stale header tap isolation passed (ASan/UBSan)')
