"""Actual HAL feedback event filter: completed taps and raw key edges only."""
from pathlib import Path
import subprocess,tempfile
s=Path('firmware/rickyos-plus-1.2/upstream/lib/hal/HalGPIO.cpp').read_text()
a=s.index('void HalGPIO::updateHapticFeedback(uint8_t enabled)');b=s.index('\nvoid HalGPIO::stopHapticFeedback()',a)
method=s[a:b]
program=r'''
#include <cassert>
#include <cstdint>
namespace plus_haptic {int pulses=0,stops=0;void pulse(){pulses++;}void stop(){stops++;}}
class HalGPIO {
public:
 enum{BTN_BACK,BTN_CONFIRM,BTN_LEFT,BTN_RIGHT,BTN_UP,BTN_DOWN,BTN_POWER};
 struct Input{bool tap=false;unsigned long held=0;bool wasTouchTap(float&,float&)const{return tap;}unsigned long lastTouchHeldMs()const{return held;}}inputMgr;
 uint8_t edges=0;bool home=false;
 bool wasHomeKeyPressed()const{return home;}bool wasPressed(uint8_t b)const{return edges&(1<<b);}
 void updateHapticFeedback(uint8_t enabled);
};
'''+method+r'''
int main(){
 HalGPIO h;
 h.inputMgr.tap=true;h.updateHapticFeedback(1);assert(plus_haptic::pulses==1);
 h.inputMgr.tap=false;h.updateHapticFeedback(1);assert(plus_haptic::pulses==1);
 h.inputMgr.tap=true;h.inputMgr.held=500;h.updateHapticFeedback(1);assert(plus_haptic::pulses==1);
 h.inputMgr.tap=false;h.inputMgr.held=0;
 for(unsigned i=0;i<6;i++){h.edges=1<<i;h.updateHapticFeedback(1);}
 assert(plus_haptic::pulses==7);
 h.edges=0;h.home=true;h.updateHapticFeedback(1);assert(plus_haptic::pulses==8);
 h.inputMgr.tap=true;h.edges=4;h.updateHapticFeedback(1);assert(plus_haptic::pulses==9);
 h.home=false;h.edges=1<<HalGPIO::BTN_POWER;h.inputMgr.tap=false;
 h.updateHapticFeedback(1);assert(plus_haptic::pulses==9);
 h.home=true;h.inputMgr.tap=true;h.updateHapticFeedback(0);
 assert(plus_haptic::pulses==9&&plus_haptic::stops==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual HAL filter passed: completed tap, six key edges, Home contact, no hold/power/release repeat, off suppression, one pulse for simultaneous events.')
