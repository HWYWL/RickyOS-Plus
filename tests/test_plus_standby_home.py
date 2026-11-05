"""Run actual input update, standby suppression and global light handler together."""
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
SRC = PORT / 'firmware/rickyos-plus-1.2/upstream/src'

def function(source, signature):
    a = source.index(signature)
    i = source.index('{', a) + 1
    depth = 1
    while depth:
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
        i += 1
    return source[a:i]

main = (SRC / 'main.cpp').read_text()
mapped = (SRC / 'MappedInputManager.cpp').read_text()
header = (SRC / 'MappedInputManager.h').read_text()
update = function(mapped, 'void MappedInputManager::update(')
suppress = function(header, 'void suppressStandbyHomeActions() const')
reset = function(header, 'void resetHomeButtonInput() const')
toggle = function(main, 'void toggleFrontlight()')
dispatch = function(main, 'if (mappedInputManager.homeButtonAction() == HomeButtonAction::ToggleFrontlight)')
hook = 'if (activityManager.standbyShowing()) mappedInputManager.suppressStandbyHomeActions();'
assert main.index('mappedInputManager.update();') < main.index(hook) < main.index('if (handleX4ProFrontlightDoubleClick())')

PROGRAM = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "HomeButtonInput.h"
#include "PlusButtonInput.h"
#define FREEINK_DEVICE_METALIO_PLUS 1
#define FREEINK_CAP_BLE_HID_HOST 0
#define LOG_INF(...) ((void)0)
uint32_t now=0;
uint32_t millis(){return now;}
struct Settings {
 uint8_t homeButtonTapAction=static_cast<uint8_t>(HomeButtonAction::Home);
 uint8_t homeButtonDoubleTapAction=static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight);
 uint8_t homeButtonLongPressAction=static_cast<uint8_t>(HomeButtonAction::Refresh);
 int frontlightOn=0,saves=0;
 void saveToFile(){++saves;}
} SETTINGS;
struct Light {
 bool available=true,lit=false;int writes=0;
 bool present()const{return available;} bool isOn()const{return lit;}
 void setOn(bool v){lit=v;++writes;}
} Frontlight;
struct Manager {bool locked=false;bool standbyShowing()const{return locked;}} activityManager;
struct Gpio {
 bool has=true,press=false,tap=false,hold=false;
 void update(){} bool hasHomeKey()const{return has;}
 bool wasHomeKeyPressed()const{return press;} bool wasHomeKeyTapped()const{return tap;}
 bool wasHomeKeyLongPressed()const{return hold;}
} gpio;
struct MappedInputManager {
 enum class Button {Back,Power,Other}; enum class SwipeDir {None,Left};
 static constexpr uint8_t kButtonCount=3;
 mutable HomeButtonInput homeButtonInput;
 mutable bool homeNeedsFreshPress=false;
 mutable HomeButtonAction homeAction=HomeButtonAction::Ignore,deferredHomeAction=HomeButtonAction::Ignore;
 mutable plus_keys::Gesture plusGestures[5];
 mutable uint16_t longPressFiredButtons=0;
 mutable int plusBackPulses=0;
 bool plusBack=false;
 HomeButtonAction plusAction=HomeButtonAction::Ignore;
 SwipeDir wasSwipe()const{return SwipeDir::None;}
 bool isPressed(Button b)const{return b==Button::Back && plusBack;}
 void pollPlusKeys()const{
   if(plusAction!=HomeButtonAction::Ignore) homeAction=plusAction;
   if(plusBack)++plusBackPulses;
 }
 HomeButtonAction homeButtonAction()const{return homeAction;}
 void update(bool deferHomeButtonAction=false)const;
 SUPPRESS
 RESET
} mappedInputManager;
'''.replace('SUPPRESS', suppress).replace('RESET', reset) + '\n' + update + '\n' + toggle + r'''
void frame(uint32_t t,bool press=false,bool tap=false,bool hold=false,bool defer=false) {
 now=t;gpio.press=press;gpio.tap=tap;gpio.hold=hold;
 mappedInputManager.update(defer);
 HOOK
 DISPATCH
}
void fresh(){mappedInputManager={};SETTINGS={};Frontlight={};activityManager={};gpio={};}
int main(){
 // An unlocked double-tap still toggles once and persists once.
 fresh();frame(10,true);frame(30,false,true);frame(100,true);frame(120,false,true);
 assert(Frontlight.lit && Frontlight.writes==1 && SETTINGS.saves==1);
 frame(500);assert(Frontlight.writes==1);
 // Many double taps, long holds and timed-out singles behind the lock: no action.
 fresh();activityManager.locked=true;
 for(uint32_t t=10;t<3000;t+=500){
   frame(t,true);frame(t+20,false,true);frame(t+100,true);frame(t+120,false,true);
   frame(t+200,false,false,true);frame(t+499);
   assert(mappedInputManager.homeAction==HomeButtonAction::Ignore);
 }
 assert(!Frontlight.lit && Frontlight.writes==0 && SETTINGS.saves==0 && SETTINGS.frontlightOn==0);
 // The guard also protects direct/global alternate shortcut callers.
 toggleFrontlight();assert(Frontlight.writes==0 && SETTINGS.saves==0);
 // A queued transfer action must be dropped, not replayed after wake.
 mappedInputManager.deferredHomeAction=HomeButtonAction::ToggleFrontlight;
 frame(3100,false,false,false,true);
 assert(mappedInputManager.deferredHomeAction==HomeButtonAction::Ignore);
 activityManager.locked=false;frame(3200);assert(Frontlight.writes==0);
 // First click before standby + second while locked cannot complete a double.
 fresh();frame(10,true);frame(30,false,true);
 activityManager.locked=true;frame(100,true);frame(120,false,true);
 activityManager.locked=false;
 mappedInputManager.resetHomeButtonInput();
 frame(400);assert(Frontlight.writes==0);
 // A contact that began while locked is released / reaches long-press after wake.
 fresh();activityManager.locked=true;frame(10,true);
 activityManager.locked=false;mappedInputManager.resetHomeButtonInput();
 frame(700,false,false,true);frame(800,false,true);frame(1200);
 assert(mappedInputManager.homeAction==HomeButtonAction::Ignore && Frontlight.writes==0);
 // The next new press is accepted; it takes two NEW taps to toggle.
 frame(1300,true);frame(1320,false,true);assert(Frontlight.writes==0);
 frame(1400,true);frame(1420,false,true);assert(Frontlight.writes==1 && Frontlight.lit);
 // Plus configurable Light uses the same global action and is blocked too.
 fresh();activityManager.locked=true;
 mappedInputManager.plusAction=HomeButtonAction::ToggleFrontlight;frame(10);
 assert(Frontlight.writes==0 && mappedInputManager.homeAction==HomeButtonAction::Ignore);
 // Crucially, the global gate does not repeatedly cancel Back/Power gestures.
 mappedInputManager.plusBack=true;frame(20);frame(30);
 assert(mappedInputManager.plusBackPulses==2 && mappedInputManager.isPressed(MappedInputManager::Button::Back));
 auto& back=mappedInputManager.plusGestures[0];
 back.update(0,false,plus_keys::Back,plus_keys::Ignore);
 back.update(10,true,plus_keys::Back,plus_keys::Ignore);
 mappedInputManager.suppressStandbyHomeActions();
 assert(back.update(30,false,plus_keys::Back,plus_keys::Ignore)==plus_keys::Back);
 // Unlocked single / long mappings remain usable; the guard is not a global disable.
 fresh();frame(10,true);frame(30,false,true);frame(400);
 assert(mappedInputManager.homeAction==HomeButtonAction::Home);
 frame(500,true);frame(1200,false,false,true);
 assert(mappedInputManager.homeAction==HomeButtonAction::Refresh);
 // Both previous frontlight preferences are untouched while locked.
 for(bool on : {false,true}) {
   fresh();activityManager.locked=true;SETTINGS.frontlightOn=on;
   frame(10,true);frame(20,false,true);frame(100,true);frame(120,false,true);
   assert(SETTINGS.frontlightOn==on && SETTINGS.saves==0 && Frontlight.writes==0);
 }
 fresh();Frontlight.available=false;toggleFrontlight();assert(Frontlight.writes==0);
}
'''.replace('HOOK', hook).replace('DISPATCH', dispatch)

with tempfile.TemporaryDirectory(prefix='plus-standby-home-') as folder:
    root = Path(folder)
    (root / 'test.cpp').write_text(PROGRAM)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(SRC / 'util'),
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Actual standby Home input: locked double/long/single/deferred/Plus Light suppressed; inherited release discarded, fresh double-tap works, Back and preferences retained')
