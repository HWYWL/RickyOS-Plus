"""Exercise the merged ReaderUtils touch policy, including the former double inversion."""
from pathlib import Path
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'
source = (APP / 'src/activities/reader/ReaderUtils.h').read_text()

def method(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
struct CrossPointSettings {enum {TAP_AND_SWIPE,TAP_ONLY,SWIPE_ONLY,INVERTED_TAP,OFF,READER_MENU_TAP};};
struct {bool touchReaderControls=true;int pageTurnDirection=0,showReaderMenu=CrossPointSettings::READER_MENU_TAP;
 int pageTurnGesture=CrossPointSettings::TAP_ONLY,previousPageGesture=CrossPointSettings::TAP_ONLY;} SETTINGS;
struct GfxRenderer {int getScreenWidth()const{return 684;}int getScreenHeight()const{return 1216;}};
struct MappedInputManager {enum class SwipeDir {None,Left,Right};int x=0,y=400;bool tapped=true,touch=true;
 SwipeDir swipe=SwipeDir::None;bool hasTouch()const{return touch;}SwipeDir wasSwipe()const{return swipe;}
 bool wasScreenTapped(int&px,int&py)const{px=x;py=y;return tapped;}};
struct {unsigned long lastTouchHeldMs()const{return 123;}} gpio;
''' + method('inline bool isInMenuBand(') + '\n' + method('inline bool gestureAllowsSwipe(') + '\n' + method('inline bool gestureAllowsTap(') + '\n' + method('struct TouchPageTurn') + ';\n' + method('inline TouchPageTurn detectTouchPageTurn(') + r'''
int main(){
 GfxRenderer renderer;MappedInputManager input;
 // Explicit expected left-edge next-page table: right/normal, right/inverted,
 // left/normal, left/inverted. Left+inverted must stay left-handed.
 const bool leftNext[2][2]={{false,true},{true,true}};
 for(int hand:{0,1})for(int inverted:{0,1})for(bool rtl:{false,true}){
  SETTINGS.pageTurnDirection=hand;
  SETTINGS.pageTurnGesture=SETTINGS.previousPageGesture=inverted?CrossPointSettings::INVERTED_TAP:CrossPointSettings::TAP_ONLY;
  input.x=0;auto left=detectTouchPageTurn(renderer,input,rtl);
  const bool expected=rtl?!leftNext[hand][inverted]:leftNext[hand][inverted];
  assert(left.next==expected&&left.prev!=expected&&left.heldMs==123);
  input.x=683;auto right=detectTouchPageTurn(renderer,input,rtl);
  assert(right.next!=expected&&right.prev==expected);
  // The top band (height/8) belongs to the reader menu, so it never turns a page.
  input.x=342;input.y=20;auto top=detectTouchPageTurn(renderer,input,rtl);
  assert(!top.prev&&!top.next);input.y=400;
 }
 SETTINGS.pageTurnGesture=SETTINGS.previousPageGesture=CrossPointSettings::SWIPE_ONLY;
 for(int hand:{0,1})for(bool rtl:{false,true})for(auto swipe:{MappedInputManager::SwipeDir::Left,MappedInputManager::SwipeDir::Right}){
  SETTINGS.pageTurnDirection=hand;input.swipe=swipe;
  const bool leftSwipe=swipe==MappedInputManager::SwipeDir::Left;
  const bool expected=(hand==int(rtl))?leftSwipe:!leftSwipe;
  auto result=detectTouchPageTurn(renderer,input,rtl);
  assert(result.next==expected&&result.prev!=expected);
 }
 input.touch=false;auto absent=detectTouchPageTurn(renderer,input);assert(!absent.prev&&!absent.next);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-holding-hand-') as directory:
    work = Path(directory)
    (work / 'check.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(work / 'check.cpp'), '-o', str(work / 'check')], check=True)
    subprocess.run([str(work / 'check')], check=True)
print('Merged actual touch policy: both hands, inverse taps, RTL, top-band menu priority, swipes and no-touch passed')
