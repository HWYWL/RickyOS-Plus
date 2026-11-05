"""Execute production static Standby render guard and wallpaper WiFi entry."""
from pathlib import Path
import subprocess
import tempfile
root=Path('firmware/rickyos-plus-1.2/upstream')
s=(root/'src/activities/apps/standby/StandbyActivity.cpp').read_text()
a=s.index('void StandbyActivity::render(RenderLock&&)')
b=s.index('\n  const auto& metrics',a)
render=s[a:b]+'\n}\n'
w=(root/'src/activities/apps/standby/RickyWallpaperDownloadActivity.cpp').read_text()
a=w.index('void RickyWallpaperDownloadActivity::onEnter()');b=w.index('\nvoid RickyWallpaperDownloadActivity::onExit()',a)
entry=w[a:b]
code=r'''
#include <cassert>
#include <cstdint>
struct Rect{int x,y,width,height;};
struct RenderLock{};
struct GfxRenderer{
 struct ClipScope{ClipScope(GfxRenderer&,int,int,int,int){}};
 int bwPaints=0;
 int getScreenWidth(){return 684;}int getScreenHeight(){return 1216;}
 void getOrientedViewableTRBL(int*a,int*b,int*c,int*d){*a=*b=*c=*d=8;}
 void clearScreen(){}void displayBuffer(int){++bwPaints;}
};
struct HalDisplay{enum{FAST_REFRESH,HALF_REFRESH};};
struct Face {
 bool clock=false,picker=false,native=true;int nativeCalls=0,bwCalls=0;
 bool wantsClock(){return clock;}bool needsPicture(){return picker;}
 bool renderNative(GfxRenderer&,const Rect&){++nativeCalls;return native;}
 void render(GfxRenderer&,const Rect&){++bwCalls;}
};
struct StandbyActivity{
 Face*currentFace_=nullptr;bool drewFrame_=false;GfxRenderer renderer;
 void render(RenderLock&&);
};
uint32_t millis(){return 0;}
constexpr int WL_CONNECTED=3;
struct{int connected=0;int status(){return connected;}}WiFi;
struct ActivityResult{bool isCancelled=false;};
struct WifiSelectionActivity{};
struct Activity{void onEnter(){}};
struct RickyWallpaperDownloadActivity:Activity{
 enum class State{Connecting,Failed};State state_=State::Failed;uint32_t enteredMs_=0;
 int selectors=0,reused=0;bool canAllocate=true;
 void requestUpdate(){}void onWifiResult(bool value){assert(value);++reused;}
 template<class T,class F>bool startActivityForResultWith(F){++selectors;return canAllocate;}
 void onEnter();
};
'''+render+entry+r'''
int main(){
 Face f;StandbyActivity a;a.currentFace_=&f;
 a.render(RenderLock{});assert(a.drewFrame_ && f.nativeCalls==1);
 for(int i=0;i<100;++i)a.render(RenderLock{});
 assert(f.nativeCalls==1 && f.bwCalls==0 && a.renderer.bwPaints==0);
 // Choosing another picture clears the latch and paints exactly once again.
 a.drewFrame_=false;a.render(RenderLock{});assert(f.nativeCalls==2);
 f.clock=true;a.render(RenderLock{});a.render(RenderLock{});assert(f.nativeCalls==4);
 f.clock=false;f.picker=true;a.render(RenderLock{});assert(f.nativeCalls==5);
 f.picker=false;f.native=false;a.drewFrame_=false;
 a.render(RenderLock{});a.render(RenderLock{});assert(f.bwCalls==1 && a.renderer.bwPaints==1);
 RickyWallpaperDownloadActivity w;WiFi.connected=WL_CONNECTED;w.onEnter();assert(w.reused==1 && w.selectors==0);
 WiFi.connected=0;w.onEnter();assert(w.selectors==1);
 w.canAllocate=false;w.onEnter();assert(w.state_==RickyWallpaperDownloadActivity::State::Failed);
}
'''
# Every face reload, including returning from the picture gallery, must release the static latch.
for marker in ('currentFace_->onEnter();\n  drewFrame_ = false;',
 'if (currentFace_) currentFace_->onEnter();\n            drewFrame_ = false;',
 'if (currentFace_) currentFace_->onEnter();\n        drewFrame_ = false;'):
 assert marker in s,marker
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Static Standby: no repeated native/BW paint, clock/picker redraw retained, picture reload reset, wallpaper WiFi reuse and OOM fallback passed')
