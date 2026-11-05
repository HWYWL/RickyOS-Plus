"""Execute the actual bounce producer/EOF consumer; preserve final portrait-edge rows."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1]
s=(root/'firmware/rickyos-plus-1.2/components/metalio_board/display/output_lcd/lcd_driver.c').read_text()
def function(signature):
 a=s.index(signature);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]
code=r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <array>
#define IRAM_ATTR
#define BOUNCE_BUF_LINES 8
#define DMA_DESCRIPTOR_BUFFER_OWNER_DMA 1
using gdma_channel_handle_t=void*;
struct gdma_event_data_t{};
using line_cb_func_t=bool(*)(void*,uint8_t*);
struct Node{struct{int owner=0;}dw0;};
struct Context {
 struct{void* dev=nullptr;}hal;void* dma_chan=nullptr;
 bool frame_finishing=false,frame_active=true;int64_t frame_t0_us=1,last_frame_us=0,frame_completed_us=0;
 line_cb_func_t line_source_cb=nullptr;void* line_cb_payload=nullptr;
 void(*frame_done_cb)(void*)=nullptr;void* frame_cb_payload=nullptr;
 int line_bytes=4,dummy_bytes=1,display_lines=688,native_height=684,scan_first_line=0;
 size_t lines_fed=0,bb_eof_count=0,bb_size=40;
 bool bb_behind_cache=false;
 uint8_t* bounce_buffer[2]{};Node dma_nodes[2];
 int64_t eof_last_us=0,eof_last_us_prev=0,eof_dt_min_us=0,eof_dt_max_us=0,eof_dt_sum_us=0;
 int line_length_us=17;
 unsigned eof_dt_n=0,eof_work_n=0;
 int64_t eof_work_min_us=0,eof_work_max_us=0,eof_work_sum_us=0;
}lcd;
struct EpdLcdRefillStats {uint32_t stale,refills;int32_t max_dt_us,max_work_us,worst_line,period_us;};
static EpdLcdRefillStats s_refill;static int64_t s_refill_worst;
inline bool border_whitening=false;
inline int64_t esp_timer_get_time(){static int64_t t=1;return ++t;}
inline int stops=0,done=0,sourceRow=0;
void lcd_ll_stop(void*){++stops;}void gdma_stop(void*){}
void epd_filter_white_border(uint8_t*,int,int,int,int,bool){}
bool source(void*,uint8_t* row){memset(row,sourceRow<lcd.native_height?sourceRow%251+1:0,4);++sourceRow;return false;}
void completed(void*){++done;}
'''
code=code.replace('int line_bytes=4,','struct {int white_border_pixels=0;}config;\n int line_bytes=4,')
for sig in ['static IRAM_ATTR void lcd_finish_frame_from_isr(', 'static IRAM_ATTR bool fill_bounce_buffer(', 'static IRAM_ATTR bool lcd_rgb_panel_eof_handler(']:code+='\n'+function(sig)+'\n'
code+=r'''
int main(){
 for(int height:{684,688,800,1216}){
  for(int dummy:{1,2}){
   lcd=Context{};lcd.native_height=height;lcd.display_lines=(height+7)/8*8;
   lcd.dummy_bytes=dummy;lcd.bb_size=8*(4+dummy);
   std::array<uint8_t,48> a{},b{};lcd.bounce_buffer[0]=a.data();lcd.bounce_buffer[1]=b.data();
   done=stops=sourceRow=0;lcd.line_source_cb=source;lcd.frame_done_cb=completed;
   fill_bounce_buffer(a.data());fill_bounce_buffer(b.data());assert(done==0);
   int sent=0;
   while(!lcd.frame_finishing){
    const int bb=lcd.bb_eof_count%2;
    for(int row=0;row<8;++row){
     const uint8_t expected=sent<height?sent%251+1:0;
     const auto* p=lcd.bounce_buffer[bb]+row*(4+dummy)+dummy%2;
     for(int x=0;x<4;++x)assert(p[x]==expected);
     ++sent;
    }
    lcd_rgb_panel_eof_handler(nullptr,nullptr,nullptr);
    assert(done==(sent>=lcd.display_lines?1:0));
   }
   assert(sent==lcd.display_lines && sourceRow==lcd.display_lines && stops==1);
   assert(!lcd.frame_active && lcd.frame_completed_us>0);
   lcd_rgb_panel_eof_handler(nullptr,nullptr,nullptr);assert(done==1 && stops==1);
  }
 }
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual LCD bounce/EOF: prefill cannot finish; every final row sent, padded HOLD, no extra source reads, once-only completion, 684/688/800/1216 rows passed')
