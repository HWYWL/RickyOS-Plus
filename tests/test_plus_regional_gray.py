"""Execute Plus driver and vendor regional submission code with fake panel I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'firmware/rickyos-plus-1.2'
driver = (PORT/'components/rickyos_display/src/driver/MetalioPlusDriver.cpp').read_text()
driver = '\n'.join(line for line in driver.splitlines() if not line.startswith('#include'))
high = (PORT/'components/metalio_board/display/highlevel.c').read_text()
high = high[high.index('static enum EpdDrawError update_region('):high.index('\nvoid epd_hl_set_all_white')]
shim = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "PlusFramebuffer.h"
#include "NativeGrayRefreshPolicy.h"
#include "GrayscaleCapabilities.h"
#include "PlusDisplayProfile.h"
extern "C" {
#include "ricky_native_turn.h"
}
static RickyNativeTurn armedTurn{}, observedTurn{};
static bool armedTextFinish=false, observedTextFinish=false;
extern "C" void epd_set_native_turn(uint8_t style, uint8_t orientation, bool right) {
 armedTurn={}; armedTurn.style=style;armedTurn.orientation=orientation;armedTurn.from_right=right;
 armedTextFinish=false;
}
extern "C" void epd_set_native_reader_text_finish(bool enabled) {armedTextFinish=enabled;}
static RickyNativeTurnRegion armedRegion{}, observedRegion{};
extern "C" void epd_set_native_ripple_origin(uint16_t x,uint16_t y) {
 armedTurn.has_origin=true;armedTurn.origin_x=x;armedTurn.origin_y=y;
}
extern "C" void epd_set_native_ui_turn(uint8_t o,bool right,int x,int y,int w,int h) {
 epd_set_native_turn(1,o,right);armedRegion={x,y,w,h,RICKY_UI_TURN_SPREAD};
}
template <class... A> static void ESP_LOGI(const char*, const char*, A...) {}
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct EpdLcdRefillStats {unsigned stale,refills; long max_dt_us,max_work_us,worst_line,period_us;};
static EpdLcdRefillStats epd_lcd_take_refill_stats() {return {};}
static int64_t esp_timer_get_time() { return 0; }
constexpr int W=1216,H=684,N=W*H/2;
struct EpdRect {int x,y,width,height;};
enum EpdDrawMode {MODE_GC16=1,MODE_DU=2,MODE_GL16=3,MODE_GL16_FAST=6,MODE_GL4=10,MODE_GL16_INV=11,MODE_GC16_FAST=12};
constexpr int MODE_PACKING_1PPB_DIFFERENCE=16;
#define MY_MODE_READER_PAPER_TURN 13
enum EpdDrawError {EPD_DRAW_SUCCESS=0,EPD_DRAW_EMPTY_LINE_QUEUE=1};
struct EpdiyHighlevelState {uint8_t *front_fb,*back_fb,*difference_fb; bool* dirty_lines; uint8_t* dirty_columns; void* waveform;};
uint8_t front[N],back[N],diff[N*2],cols[W/2]; bool lines[H];
EpdiyHighlevelState hl{front,back,diff,lines,cols,nullptr};
struct Call {EpdRect area; bool cleanup; int mode;};
std::vector<Call> calls;
int powered=0,off=0,full=0,cleared=0,failDraw=0;
std::vector<int> selectedModes;
int epd_width(){return W;} int epd_height(){return H;}
EpdRect epd_full_screen(){return {0,0,W,H};}
EpdRect _inverse_rotated_area(int x,int y,int w,int h){return {x,y,w,h};}
EpdRect epd_difference_image_cropped(const uint8_t* to,const uint8_t* from,EpdRect a,uint8_t*,bool* dirty,uint8_t* columns){
 memset(dirty,0,H); memset(columns,0,W/2); bool changed=false;
 for(int y=a.y;y<a.y+a.height;++y)for(int x=0;x<W/2;++x){
  uint8_t d=to[y*W/2+x]^from[y*W/2+x]; if(!d)continue;
  dirty[y]=true; columns[x]|=d;
  if(x*2>=a.x && x*2<a.x+a.width)changed=true;
 }
 return changed?a:EpdRect{};
}
EpdDrawError epd_draw_base(EpdRect,const uint8_t*,EpdRect a,int mode,int,bool*,uint8_t* columns,void*){
 assert(a.x==0 && a.y==0 && a.width==W && a.height==H);
 bool any=false;for(int x=0;x<W/2;++x)any|=columns[x]!=0;assert(any);
 observedTurn=armedTurn;observedRegion=armedRegion;
 calls.push_back({a,(mode&15)==MODE_GC16,mode&15});
 if(failDraw && --failDraw==0)return EPD_DRAW_EMPTY_LINE_QUEUE;
 return EPD_DRAW_SUCCESS;
}
'''
backend = r'''
bool MetalioEpd_Init(){memset(front,255,N);memset(back,255,N);return true;}
EpdiyHighlevelState* MetalioEpd_Hl(){return &hl;}
void epd_poweron(){++powered;} void epd_poweroff(){++off;}
bool epd_board_metalio_eink4_plus_tps_ok(){return true;}
EpdDrawError epd_clear_checked(){++cleared;return EPD_DRAW_SUCCESS;}
EpdDrawError epd_hl_update_screen_from_white(EpdiyHighlevelState*,EpdDrawMode m,int){selectedModes.push_back(m);++full;memcpy(back,front,N);return EPD_DRAW_SUCCESS;}
EpdDrawError epd_hl_update_screen_full(EpdiyHighlevelState*,EpdDrawMode m,int){selectedModes.push_back(m);++full;memcpy(back,front,N);return EPD_DRAW_SUCCESS;}
EpdRect readerBody{};
int readerSubmissions=0, failReader=0;
EpdDrawError epd_hl_update_screen_reader(EpdiyHighlevelState*,int,EpdRect body){
 observedTurn=armedTurn;
 observedTextFinish=armedTextFinish;
 readerBody=body;++readerSubmissions;selectedModes.push_back(MODE_GC16_FAST);
 if(failReader)return EPD_DRAW_EMPTY_LINE_QUEUE;
 memcpy(back,front,N);return EPD_DRAW_SUCCESS;
}
EpdDrawError epd_hl_update_screen_reader_paper(EpdiyHighlevelState* s,int t,EpdRect body){
 auto result=epd_hl_update_screen_reader(s,t,body);
 selectedModes.back()=MY_MODE_READER_PAPER_TURN;return result;
}
EpdDrawError epd_hl_update_screen(EpdiyHighlevelState* s,EpdDrawMode m,int t){selectedModes.push_back(m);return epd_hl_update_area(s,m,t,epd_full_screen());}
namespace freeink {
struct EpdBus{};
enum class BusyPolarity {ActiveHigh}; enum class OpticalState {Unknown,Bw,Gray}; enum class RefreshMode{Full,Half,Fast};
struct PanelGeometry {uint16_t width,height,widthBytes;uint32_t bufferSize;};
class PanelDriver {
public:
 virtual ~PanelDriver()=default;
 virtual uint32_t spiHz()const=0; virtual BusyPolarity busyPolarity()const=0; virtual PanelGeometry geometry()const=0;
 virtual bool usesExternalBus()const=0;virtual void begin(EpdBus&)=0;virtual void deepSleep(EpdBus&)=0;
 virtual OpticalState opticalState()const=0;
 virtual void setBackgroundHint(bool){}
 virtual void display(EpdBus&,const uint8_t*,const uint8_t*,RefreshMode,bool)=0;
 virtual uint8_t* beginGrayscale16()=0;virtual bool commitGrayscale16(const uint8_t*)=0;
 virtual void cancelGrayscale16()=0;virtual void requestResync(uint8_t)=0;
 virtual GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode=GrayscaleMode::Overlay)const{return {};}
 virtual void copyGrayscaleLsb(EpdBus&,const uint8_t*){}
 virtual void copyGrayscaleMsb(EpdBus&,const uint8_t*){}
 virtual void displayGray(EpdBus& bus,const uint8_t* fb,bool off,const unsigned char*,bool){
   display(bus,fb,nullptr,RefreshMode::Fast,off);
 }
 virtual void cleanupGrayscaleBuffers(EpdBus&,const uint8_t*){}
};
}
'''
main = r'''
int main(){
 // Same-row disjoint submissions must not publish the second zone early.
 memset(front,255,N);memset(back,255,N);front[0]=0;front[W/2-1]=0;
 assert(epd_hl_update_area(&hl,MODE_GL16,25,{0,0,32,1})==EPD_DRAW_SUCCESS);
 assert(back[0]==0 && back[W/2-1]==255);
 assert(epd_hl_update_area(&hl,MODE_GL16,25,{W-32,0,32,1})==EPD_DRAW_SUCCESS);
 assert(back[W/2-1]==0);
 front[0]=0x88; failDraw=1;
 assert(epd_hl_update_area_full(&hl,MODE_GC16,25,{0,0,32,1})!=EPD_DRAW_SUCCESS);
 assert(back[0]==0);
 // Actual Plus driver: startup, no-op, regional cleanup, cancellation, recovery.
 using namespace freeink;
 EpdBus bus;auto& d=metalioPlusDriver();d.begin(bus);
 uint8_t proxy[W*H/8];memset(proxy,255,sizeof(proxy));
 assert(!metalioPlusRestoreGray16Baseline(proxy));
 metalioPlusSetGray16Profile(1);
 d.beginGrayscale16();front[0]=0;assert(d.commitGrayscale16(proxy));assert(cleared==1);
 assert(selectedModes.back()==MODE_GC16); // Unknown always restores the optical baseline.
 const auto submissions=selectedModes.size();
 const int fullBefore=full;
 d.beginGrayscale16();memcpy(front,back,N);front[1]=0x88;
 metalioPlusSetGray16Profile(1);assert(d.commitGrayscale16(proxy));
 // An ordinary reader turn drives every row and column with the vendor GL16 (faint-ink fix).
 assert(selectedModes.size()==submissions+1 && selectedModes.back()==MODE_GC16_FAST && full==fullBefore+1);
 // An explicit reader body is one-shot and never changes the cleanup policy.
 d.beginGrayscale16();memcpy(front,back,N);front[1]=0x89;
 metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);
 metalioPlusSetPageTurn(2,2,true);
 metalioPlusSetGray16ReaderTextFinish(true);
 assert(d.commitGrayscale16(proxy));assert(readerSubmissions==1 && readerBody.x==96);
 assert(observedTextFinish && !armedTextFinish && !nextReaderTextFinish);
 assert(observedTurn.style==2 && observedTurn.orientation==2 && observedTurn.from_right && !armedTurn.style);
 assert(readerBody.width==W-192 && readerBody.height==H-64);
 // Completion-only text remains available with spatial motion OFF.
 d.beginGrayscale16();memcpy(front,back,N);front[1]=0x87;
 metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);
 metalioPlusSetGray16ReaderTextFinish(true);
 assert(d.commitGrayscale16(proxy) && observedTextFinish && !observedTurn.style);
 const int textSubmissions=readerSubmissions;assert(textSubmissions==2);
 d.beginGrayscale16();memcpy(front,back,N);front[1]=0x88;
 metalioPlusSetGray16Profile(1);assert(d.commitGrayscale16(proxy));assert(readerSubmissions==textSubmissions);
 metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);
 metalioPlusSetGray16ReaderTextFinish(true);
 metalioPlusSetGray16Profile(2);assert(d.commitGrayscale16(proxy));assert(readerSubmissions==textSubmissions);
 assert(!nextReaderTextFinish && !armedTextFinish);
 assert(selectedModes.back()==MODE_GC16);
 metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);metalioPlusSetGray16ReaderTextFinish(true);d.cancelGrayscale16();
 assert(!nextReaderTextFinish);
 assert(d.commitGrayscale16(proxy));assert(readerSubmissions==textSubmissions);
 d.beginGrayscale16();memcpy(front,back,N);front[1]=0x99;
 metalioPlusSetGray16Profile(2);assert(d.commitGrayscale16(proxy));
 assert(selectedModes.back()==MODE_GC16); // Cycle/manual cleanup is never suppressed.
 d.beginGrayscale16();memcpy(front,back,N);front[1]=0xaa;
 assert(d.commitGrayscale16(proxy));assert(selectedModes.back()==MODE_GC16); // Profile is one-shot.
 // Reading-background profile survives the geometry hint but never inherits the
 // white-text convergence timing. Spatial motion remains available, AA or not.
 for(int motion:{0,1,2,3})for(bool finish:{false,true}){
   d.beginGrayscale16();memcpy(front,back,N);front[1]=0x89;
   metalioPlusSetGray16Profile(5);
   metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);
   assert(nextGrayProfile==5);
   metalioPlusSetPageTurn(motion,0,true);
   metalioPlusSetGray16ReaderTextFinish(finish);
   assert(d.commitGrayscale16(proxy));
   assert(selectedModes.back()==MY_MODE_READER_PAPER_TURN && !observedTextFinish);
   assert(observedTurn.style==motion && !armedTurn.style);
   assert(readerBody.x==96 && readerBody.width==W-192);
   assert(nextGrayProfile==0 && !nextReaderTextFinish);
 }
 metalioPlusSetGray16Profile(5);d.cancelGrayscale16();
 assert(d.commitGrayscale16(proxy));assert(selectedModes.back()==MODE_GC16);
 metalioPlusSetGray16Profile(5);metalioPlusSetGray16ReaderRefresh(0,0,W,H);
 metalioPlusSetGray16Profile(2);assert(d.commitGrayscale16(proxy));
 assert(selectedModes.back()==MODE_GC16);
 int prior=powered;
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,1,4);
 d.beginGrayscale16();memcpy(front,back,N);assert(d.commitGrayscale16(proxy));assert(powered==prior);
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,1,4);
 d.beginGrayscale16();memcpy(front,back,N);front[0]=0x88;front[100]=0;front[W/2-1]=0;
 calls.clear();assert(d.commitGrayscale16(proxy));assert(calls.size()==2);
 assert(calls[0].cleanup && !calls[1].cleanup);assert(back[0]==0x88 && back[W/2-1]==0);
 assert(calls[1].mode==MODE_GL16_INV); // An ordinary UI step takes the UI-turn table.
 d.beginGrayscale16();assert(front[0]==255);assert(metalioPlusRestoreGray16Baseline(proxy));assert(front[0]==0x88);
 front[0]=0;metalioPlusSetGray16UiRefresh(0,0,0,0,1,1);d.cancelGrayscale16();assert(front[0]==0x88);
 assert(proxy[0]==0xff); // >=8 gray proxy is white.
 // Failure on second regional scan invalidates the whole optical baseline.
 d.beginGrayscale16();memcpy(front,back,N);front[0]=0;front[100]=0x88;front[W/2-1]=0x88;
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,1,4);failDraw=2;
 assert(!d.commitGrayscale16(proxy));assert(d.opticalState()==OpticalState::Unknown);
 assert(!metalioPlusRestoreGray16Baseline(proxy));
 d.beginGrayscale16();front[1]=0;metalioPlusSetGray16UiRefresh(0,0,0,0,8,32);
 assert(d.commitGrayscale16(proxy));assert(cleared==2);assert(powered==off);
 // Ripple origin reaches the physical reader submission as one request.
 d.beginGrayscale16();memcpy(front,back,N);front[1]^=0x11;
 metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);
 metalioPlusSetPageTurn(3,0,false,371,693);
 assert(d.commitGrayscale16(proxy));
 assert(observedTurn.style==3 && observedTurn.has_origin && observedTurn.origin_x==371 && observedTurn.origin_y==693);
 assert(!armedTurn.style);
 // Main tab: before the only regional scan, content wavefront is armed.
 d.beginGrayscale16();memcpy(front,back,N);front[2]^=0x11;
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,8,32,true);
 metalioPlusSetMainTabTurn(2,true,7,9,W-60,H-20);
 calls.clear();assert(d.commitGrayscale16(proxy));assert(calls.size()==1);
 assert(!calls[0].cleanup && calls[0].mode==MODE_GL16_INV);
 assert(observedTurn.style==1 && observedTurn.orientation==2 && observedTurn.from_right);
 assert(observedRegion.x==7 && observedRegion.width==W-60 && !armedTurn.style);
 // Cancelled/manual-clean requests cannot leak their wavefront into another frame.
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,8,32,true);
 metalioPlusSetMainTabTurn(0,true,0,0,W,H);d.cancelGrayscale16();
 d.beginGrayscale16();memcpy(front,back,N);front[2]^=0x11;
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,8,32,true);
 assert(d.commitGrayscale16(proxy));assert(!observedTurn.style);
 metalioPlusSetGray16UiRefresh(W-32,0,32,H,8,32,true);
 metalioPlusSetMainTabTurn(0,true,0,0,W,H);metalioPlusSetGray16Profile(2);
 assert(d.commitGrayscale16(proxy));assert(selectedModes.back()==MODE_GC16 && !armedTurn.style);
 assert(powered==off);
 // New gray is never reduced to DU and ledger uses constant bounded storage.
 // Two 64-bit gray-travel counters add 16 bytes; still no heap/snapshot.
 static_assert(sizeof(ricky::gray::Ledger)<=80);
 // Reproduce EPUB PNG / mixed-page fallback. The argument to displayGray is
 // the final selector mask, NOT a B/W page. The default PanelDriver method
 // would display it and turn every untouched white margin black.
 uint8_t lsb[W*H/8]{},msb[W*H/8]{};
 memset(proxy,255,sizeof(proxy));proxy[0]=0x14;
 lsb[0]=0x41;msb[0]=0x63;
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
 d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleMsb(bus,msb);
 d.displayGray(bus,msb,false,nullptr,false);
 const uint8_t levels[]{0,5,10,15,0,15,10,5};
 for(unsigned x=0;x<8;++x)assert(((front[x/2]>>((x&1)*4))&15)==levels[x]);
 for(int i=4;i<N;++i)assert(front[i]==255);
 assert(d.opticalState()==OpticalState::Gray && memcmp(front,back,N)==0);
 assert(d.grayscaleCapabilities().supported());
 assert(d.grayscaleCapabilities().encoding==GrayscaleEncoding::OverlayMasks);
 assert(d.grayscaleCapabilities().base==GrayscaleBase::Separate);
 assert(!d.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
 assert(!d.grayscaleCapabilities(GrayscaleMode::Direct).supported());
 // Missing, out-of-order, duplicate or cancelled planes never submit a mask.
 for(int reason=0;reason<8;++reason){
  d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
  int before=powered;
  if(reason==0){} // No upload.
  if(reason==1)d.copyGrayscaleMsb(bus,msb);
  if(reason>=2)d.copyGrayscaleLsb(bus,lsb);
  if(reason==3)d.copyGrayscaleLsb(bus,nullptr);
  if(reason==4)d.copyGrayscaleMsb(bus,nullptr);
  if(reason>=5)d.copyGrayscaleMsb(bus,msb);
  if(reason==5)d.copyGrayscaleMsb(bus,msb);
  if(reason==6)d.cleanupGrayscaleBuffers(bus,proxy);
  if(reason==7){const unsigned char lut=0;d.displayGray(bus,msb,false,&lut,false);}
  d.displayGray(bus,msb,false,nullptr,false);
  assert(powered==before && memcmp(front,back,N)==0 && d.opticalState()==OpticalState::Bw);
 }
 // A fresh LSB replaces an unfinished transaction, and native begin / resync
 // invalidates the old overlay so it cannot be committed into another page.
 d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleMsb(bus,msb);
 d.displayGray(bus,msb,false,nullptr,false);assert(d.opticalState()==OpticalState::Gray);
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
 // B/W HALF stays DU on a light page (it cleans through the 16-gray path); Night
 // mode is B/W only, so its HALF (due page, Refresh screen) must clean with GC16.
 d.display(bus,proxy,nullptr,RefreshMode::Half,false);assert(selectedModes.back()==MODE_DU);
 d.setBackgroundHint(true);
 d.display(bus,proxy,nullptr,RefreshMode::Half,false);assert(selectedModes.back()==MODE_GC16);
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);assert(selectedModes.back()==MODE_DU);
 d.setBackgroundHint(false);
 d.copyGrayscaleLsb(bus,lsb);d.beginGrayscale16();prior=powered;
 d.displayGray(bus,msb,false,nullptr,false);assert(powered==prior);
 d.cancelGrayscale16();assert(memcmp(front,back,N)==0);
 d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleMsb(bus,msb);d.requestResync(1);
 prior=powered;d.displayGray(bus,msb,false,nullptr,false);assert(powered==prior);
 // A panel failure does not publish a successful optical baseline. The next
 // normal base recovers through the checked clear path before another overlay.
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
 d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleMsb(bus,msb);failDraw=1;
 d.displayGray(bus,msb,false,nullptr,false);assert(d.opticalState()==OpticalState::Unknown);
 int previousClear=cleared;
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);assert(cleared==previousClear+1);
 d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleMsb(bus,msb);
 d.displayGray(bus,msb,false,nullptr,true);assert(d.opticalState()==OpticalState::Gray && powered==off);
 // A failed text-finish transaction clears both one-shot hints and refuses to
 // publish its pixels; recovery is the original GC path, not a leaked effect.
 d.beginGrayscale16();memcpy(front,back,N);front[8]^=255;const auto oldPixel=back[8];
 metalioPlusSetGray16ReaderRefresh(96,32,W-192,H-64);metalioPlusSetGray16ReaderTextFinish(true);
 metalioPlusSetPageTurn(3,0,true);failReader=1;
 assert(!d.commitGrayscale16(proxy));
 assert(d.opticalState()==OpticalState::Unknown && back[8]==oldPixel);
 assert(!nextReaderTextFinish && !armedTextFinish && !armedTurn.style);
 failReader=0;d.cancelGrayscale16();
 assert(d.commitGrayscale16(proxy) && selectedModes.back()==MODE_GC16 && powered==off);
}
'''
def run_program(program):
    with tempfile.TemporaryDirectory(prefix='plus-regional-test-') as folder:
        p=Path(folder);(p/'test.cpp').write_text(program)
        subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-I',str(PORT/'components/metalio_board/display'),'-I',str(PORT/'components/rickyos_display/include'),'-I',str(PORT/'upstream/lib/hal'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)


if __name__ == '__main__':
    run_program(shim+high+backend+driver+main)
    print('Plus actual driver: regional submissions, legacy image-page polarity, interrupted overlays and recovery passed (ASan/UBSan)')
