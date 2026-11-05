"""Actual Plus gesture classifier + actual MappedInputManager routing under ASan/UBSan."""
from pathlib import Path
import subprocess, tempfile
u=Path('firmware/rickyos-plus-1.2/upstream')
source=(u/'src/MappedInputManager.cpp').read_text()
def extract(signature):
 start=source.index(signature); cursor=source.index('{',start)+1; depth=1
 while depth:
  depth+=(source[cursor]=='{')-(source[cursor]=='}'); cursor+=1
 return source[start:cursor]
methods='\n'.join(extract(x) for x in ['bool MappedInputManager::mapButton(', 'bool MappedInputManager::physicalButton(', 'void MappedInputManager::pollPlusKeys()', 'bool MappedInputManager::wasPressed(', 'bool MappedInputManager::wasReleased('])
program=r'''
#include <cassert>
#include <cstdint>
#include <array>
#include <utility>
#include "PlusButtonInput.h"
#define FREEINK_DEVICE_METALIO_PLUS 1
#define FREEINK_CAP_TOUCH 0
uint32_t now=0;
uint32_t millis(){return now;}
struct HalGPIO {
 enum {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER};
 uint8_t down=0,press=0,release=0;
 bool wasPressed(uint8_t k)const{return press&(1<<k);}
 bool wasReleased(uint8_t k)const{return release&(1<<k);}
 bool isPressed(uint8_t k)const{return down&(1<<k);}
 void set(uint8_t bits){press=bits&~down;release=down&~bits;down=bits;}
};
struct CrossPointSettings {
 enum {PREV_NEXT,NEXT_PREV,SIDE_BUTTONS_DISABLED,NEXT_NEXT,PREV_PREV};
 uint8_t frontButtonBack=0,frontButtonConfirm=1,frontButtonLeft=2,frontButtonRight=3,sideButtonLayout=0;
 uint8_t plusKeyActions[10]={0,1,0,1,0,1,0,1,0,1};
} SETTINGS;
enum class HomeButtonAction {Ignore,Home,ReaderMenu,Refresh,ToggleFrontlight,Confirm,Back};
class MappedInputManager {
 public:
 enum class Button {Back,Confirm,Left,Right,Up,Down,Power,PageBack,PageForward,NavNext,NavPrevious,ScreenLeft,ScreenRight,ScreenUp,ScreenDown,Count};
 static constexpr int kButtonCount=15;
 HalGPIO gpio;
 mutable plus_keys::Gesture plusGestures[5];
 mutable uint16_t plusActions=0;
 mutable uint8_t plusBlocked=0,plusDefaultPulse=0;
 mutable bool plusControl=false;
 mutable HomeButtonAction homeAction=HomeButtonAction::Ignore;
 std::array<bool,15>blePressEdges{},bleReleaseEdges{};
 bool bleEdge(const std::array<bool,15>&,Button)const{return false;}
 bool wasBackGesture()const{return false;}
 bool isNavDirectionSwapped()const{return false;}
 Button mapScreenDirection(Button)const{return Button::Left;}
 bool mapButton(Button,bool(HalGPIO::*)(uint8_t)const)const;
 bool physicalButton(uint8_t,bool(HalGPIO::*)(uint8_t)const)const;
 void pollPlusKeys()const;
 bool wasPressed(Button)const;
 bool wasReleased(Button)const;
 void frame(uint32_t t,uint8_t bits){now=t;gpio.set(bits);homeAction=HomeButtonAction::Ignore;pollPlusKeys();}
};
'''+methods+r'''
int main(){
 using namespace plus_keys;
 using B=MappedInputManager::Button;
 Gesture g;
 assert(g.update(0,false,Next,Home)==Count);
 assert(g.update(10,true,Next,Home)==Count && g.blocked);
 assert(g.update(609,true,Next,Home)==Count);
 assert(g.update(610,true,Next,Home)==Home);
 assert(g.update(900,true,Next,Home)==Count);
 assert(g.update(950,false,Next,Home)==Count && g.blocked);
 assert(g.update(1000,true,Next,Home)==Count);
 assert(g.update(1100,false,Next,Home)==Next);
 // Snapshot mapping at contact start; a settings edit cannot change its release.
 g.update(1200,true,Back,Ignore);
 assert(g.update(1300,false,Confirm,Ignore)==Back);
 g.update(1400,true,Next,Home);g.cancel();
 assert(g.update(2000,true,Next,Home)==Count);
 assert(g.update(2100,false,Next,Home)==Count);
 Gesture boot;
 assert(boot.update(1,true,Next,Home)==Count);
 assert(boot.update(1000,true,Next,Home)==Count);
 assert(boot.update(1100,false,Next,Home)==Count);
 assert(valid(255,false)==Default && valid(255,true)==Ignore);
 // millis rollover.
 Gesture wrap;wrap.update(0,false,Next,Home);wrap.update(UINT32_MAX-100,true,Next,Home);
 assert(wrap.update(499,true,Next,Home)==Home);
 MappedInputManager m;m.frame(0,0);
 // Default AI remains ordinary Confirm; power is never captured.
 m.frame(1,2|64);assert(m.wasPressed(B::Confirm)&&m.wasPressed(B::Power));
 m.frame(2,0);assert(m.wasReleased(B::Confirm)&&m.wasReleased(B::Power));
 SETTINGS.plusKeyActions[0]=Next;SETTINGS.plusKeyActions[1]=Home;
 m.frame(10,2);assert(!m.wasPressed(B::Confirm)&&!m.wasReleased(B::PageForward));
 m.frame(20,0);assert(!m.wasReleased(B::Confirm)&&m.wasReleased(B::PageForward)&&m.wasReleased(B::Down)&&m.wasPressed(B::NavNext));
 m.frame(30,0);assert(!m.wasReleased(B::PageForward));
 m.frame(100,2);m.frame(700,2);assert(m.homeAction==HomeButtonAction::Home&&!m.wasReleased(B::Confirm));
 m.frame(701,2);assert(m.homeAction==HomeButtonAction::Ignore);
 m.frame(800,0);assert(!m.wasReleased(B::PageForward)&&!m.wasReleased(B::Confirm));
 // Default short with a configured hold delays Confirm and releases only once.
 SETTINGS.plusKeyActions[0]=Default;SETTINGS.plusKeyActions[1]=Control;
 m.frame(900,2);assert(!m.wasPressed(B::Confirm));
 m.frame(1000,0);assert(m.wasPressed(B::Confirm)&&m.wasReleased(B::Confirm));
 m.frame(1100,2);m.frame(1700,2);assert(m.plusControl&&!m.wasPressed(B::Confirm));
 m.frame(1800,0);assert(!m.wasReleased(B::Confirm));
 // Every key independently maps; disabled short consumes both raw edges.
 constexpr uint8_t keys[]={1,2,3,4,5};
 for(unsigned i=0;i<5;i++){
  SETTINGS.plusKeyActions[2*i]=Back;SETTINGS.plusKeyActions[2*i+1]=Ignore;
  m.frame(2000+i*100,1<<keys[i]);assert(!m.wasPressed(B::Back));
  m.frame(2050+i*100,0);assert(m.wasPressed(B::Back)&&m.wasReleased(B::Back));
 }
 SETTINGS.plusKeyActions[0]=Ignore;m.frame(3000,2);m.frame(3010,0);
 assert(!m.wasPressed(B::Confirm)&&!m.wasReleased(B::Confirm)&&m.plusActions==0);
 for (auto pair : {std::pair<uint8_t,HomeButtonAction>{Home,HomeButtonAction::Home},
                   {Menu,HomeButtonAction::ReaderMenu},{Refresh,HomeButtonAction::Refresh},
                   {Light,HomeButtonAction::ToggleFrontlight}}) {
  SETTINGS.plusKeyActions[0]=pair.first;m.frame(3020,2);m.frame(3030,0);
  assert(m.homeAction==pair.second);m.frame(3040,0);assert(m.homeAction==HomeButtonAction::Ignore);
 }
 // Side default keeps the user's existing reversed page layout.
 SETTINGS.plusKeyActions[6]=Default;SETTINGS.sideButtonLayout=CrossPointSettings::NEXT_PREV;
 m.frame(3100,16);m.frame(3110,0);assert(m.wasReleased(B::PageForward));
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-g','-I'+str(u/'src/util'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual Plus gestures and logical routing passed: five keys, defaults, tap/hold exclusivity, transition cancel, boot hold, rollover, invalid settings, power preservation.')
