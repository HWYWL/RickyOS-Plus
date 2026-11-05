"""Boot-only full-panel exercise: bounded, no storage/heap, stop on driver error."""
from pathlib import Path
import subprocess,tempfile
PORT=Path(__file__).resolve().parents[1]
s=(PORT/'firmware/rickyos-plus-1.2/components/rickyos_display/src/driver/MetalioPlusDriver.cpp').read_text()
a=s.index('  void runTurnSelfTest() {');b=s.index('\n#endif',a)
actual=s[a:b]
with tempfile.TemporaryDirectory(prefix='plus-turn-selftest-') as d:
 p=Path(d)
 (p/'test.cpp').write_text(r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#define ESP_LOGI(tag,...) do{if(false)std::printf(__VA_ARGS__);}while(0)
enum EpdDrawError{EPD_DRAW_SUCCESS=0,EPD_DRAW_EMPTY_LINE_QUEUE=0x400};
enum OpticalState{Unknown,Gray};
constexpr int MODE_GC16=2;
struct Area{};
struct HL{uint8_t* front_fb;};
static uint8_t frame[1216*684/2];static HL hl{frame};
HL* MetalioEpd_Hl(){return &hl;}
static int calls=0,fail=-1,armed=0,cleared=0;static bool selected=false;
void epd_set_native_turn(int style,int orientation,bool){
 if(!selected){assert(style==calls/8&&orientation>=0&&orientation<4);++armed;selected=true;}else {assert(style==0);++cleared;selected=false;}
}
void epd_set_native_ripple_origin(int x,int y){assert((x==0||x==1023)&&y==512);}
void epd_set_native_reader_text_finish(bool b){assert(b);}
static int64_t esp_timer_get_time(){return 0;}
struct Driver{
 OpticalState state_=Gray;
 struct Geometry{unsigned bufferSize=103968;};Geometry geometry(){return {};}
 EpdDrawError refresh(HL* h,int mode,bool full){assert(h==&hl&&mode==2&&full);for(auto v:frame)assert(v==255);return fail==0?EPD_DRAW_EMPTY_LINE_QUEUE:EPD_DRAW_SUCCESS;}
 EpdDrawError refreshTurn(HL* h,Area,bool paper){assert(h==&hl&&!paper);++calls;bool ink=false,white=false,gray=false;for(auto v:frame){ink|=(v&15)==0;white|=(v&15)==15;gray|=(v&15)>0&&(v&15)<15;}assert(ink&&white);assert(gray==((calls-1)%2==1));return calls==fail?EPD_DRAW_EMPTY_LINE_QUEUE:EPD_DRAW_SUCCESS;}
"""+actual+r"""
};
int main(){
 for(int failure=-1;failure<=32;failure++){
 calls=armed=cleared=0;selected=false;fail=failure;Driver d;d.runTurnSelfTest();
 int expected=failure<0?32:failure;assert(calls==expected&&armed==expected&&cleared==expected);
 if(failure!=0)assert(d.state_==Unknown);
 }
 puts("Boot turn selftest: actual1216x684 frame,32 bounded OFF/skim/arc/ripple variants,AA/binary patterns,orientation/origin,stop-first-error,no heap/storage passed");
}
""")
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
