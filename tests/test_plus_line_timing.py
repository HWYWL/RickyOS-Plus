"""Execute actual timing planner, idle setter/RMT builder and render-loop policy."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LCD = ROOT / 'firmware/rickyos-plus-1.2/components/metalio_board/display/output_lcd'


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


def run(code, flags=()):
    with tempfile.TemporaryDirectory(prefix='plus-line-timing-') as folder:
        p = Path(folder)
        (p / 'test.cpp').write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', *flags, '-I', str(LCD),
                        str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
        subprocess.run([str(p / 'test')], check=True)


driver = r'''
#include <cassert>
#include <algorithm>
#include <cstdint>
#include "line_timing.h"
using std::min; using std::max;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define taskENTER_CRITICAL(p) (++locked)
#define taskEXIT_CRITICAL(p) (--locked)
#define RMT_CKV_CHAN 1
int locked=0,stopped=0,RMT=0;
int64_t now=1000;
int64_t esp_timer_get_time(){return now;}
void esp_rom_delay_us(int n){now+=n;}
void rmt_ll_tx_stop(int*,int){++stopped;}
struct rmt_item32_t {int duration0=0,level0=0,duration1=0,level1=0,val=0;};
struct {struct{rmt_item32_t data32[2];}chan[2];}RMTMEM;
struct {
 int ckv_period_us=17,ckv_period_ticks=0;uint32_t ckv_tick_hz=10000000;
 int line_cycles=340,default_line_cycles=340,default_ckv_high_us10=159;
 int lcd_res_h=304,line_length_us=17;
 bool frame_active=false;int64_t frame_completed_us=0;
 struct{int pixel_clock=20000000,le_high_time=9,line_front_porch=4;
        int dummy_clocks_after_data=10,ckv_high_time=159;}config;
}lcd;
''' + function(LCD / 'lcd_driver.c', 'static void ckv_rmt_build_signal(') + '\n' + function(
    LCD / 'lcd_driver.c', 'bool epd_lcd_set_line_timing(') + r'''
int main(){
 EpdLcdLineTiming t{};
 assert(epd_lcd_plan_line_timing(20000000,10000000,338,340,159,169,20,&t));
 assert(t.cycles==338 && t.period_ticks==169 && t.high_ticks==20);
 assert(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,120,20,&t));
 assert(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,170,9,&t));
 assert(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,170,160,&t));
 assert(!epd_lcd_plan_line_timing(0,10000000,338,340,159,170,20,&t));
 assert(!epd_lcd_plan_line_timing(20000000,0,338,340,159,170,20,&t));
 assert(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,-1,20,&t));
 assert(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,1001,20,&t));
 assert(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,170,20,nullptr));
 // Non-integral clock ratio: exact common quantum, never independent rounding.
 assert(epd_lcd_plan_line_timing(24000000,10000000,396,408,159,171,20,&t));
 assert(t.cycles%12==0 && t.period_ticks*24000000LL==t.cycles*10000000LL);
 for(int period=20;period<=1000;++period)for(int high=10;high<=159;++high){
   if(!epd_lcd_plan_line_timing(20000000,10000000,338,340,159,period,high,&t))continue;
   assert(t.cycles>=338 && t.high_ticks>=10 && t.period_ticks-t.high_ticks>=10);
   assert(t.period_ticks*20000000LL==t.cycles*10000000LL);
 }
 assert(epd_lcd_set_line_timing(170,20));
 assert(lcd.line_cycles==340 && lcd.ckv_period_ticks==170 && stopped==1 && locked==0);
 assert(RMTMEM.chan[1].data32[0].duration0==20);
 assert(RMTMEM.chan[1].data32[0].duration1==150);
 assert(epd_lcd_set_line_timing(170,20) && stopped==1); // No live memory rewrite per phase.
 lcd.frame_active=true;
 assert(!epd_lcd_set_line_timing(0,0) && stopped==1 && lcd.config.ckv_high_time==20);
 lcd.frame_active=false;lcd.frame_completed_us=now;
 assert(epd_lcd_set_line_timing(0,0) && now>=1102 && stopped==2);
 assert(lcd.line_cycles==340 && lcd.config.ckv_high_time==159);
 assert(RMTMEM.chan[1].data32[0].duration0==159);
 assert(RMTMEM.chan[1].data32[0].duration1==11);
 assert(!epd_lcd_set_line_timing(120,20));
 assert(lcd.line_cycles==340 && lcd.config.ckv_high_time==159 && locked==0);
 assert(epd_lcd_set_line_timing(169,20));
 assert(lcd.line_cycles==338 && RMTMEM.chan[1].data32[0].duration1==149);
 assert(epd_lcd_set_line_timing(0,0) && lcd.line_cycles==340);
}
'''
run(driver)

render = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <utility>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define IRAM_ATTR
#define EPD_MIN_PHASE_US 12000
#define NUM_RENDER_THREADS 2
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define EPD_DRAW_EMPTY_LINE_QUEUE 0x400
#define EPD_DRAW_NO_PHASES_AVAILABLE 0x100
#define MODE_GL4 10
#include "lcd_fault.h"
static uint32_t s_first_fault;
uint32_t epd_lcd_get_first_fault(){return s_first_fault;}
using frame_done_func_t=void(*)(void*);
struct RenderContext_t {
 int cycle_frames=2,lines_total=688,mode=6,current_frame=0,error=0,frame_time=0;
 const int* phase_times=nullptr;
 int feed_tasks[2]{},frame_done=0,feed_done_smphr[2]{},line_queues[2]{};
};
int last_phase_average=0,last_phase_minimum=0,last_phase_maximum=0;
int64_t now=1;
int64_t esp_timer_get_time(){now+=12000;return now;}
void pace_phase(int64_t){}
std::vector<std::pair<int,int>> requests;
int modeState=0,feeds=0,rejectAt=-1,failWait=0;
bool epd_lcd_set_line_timing(int line,int gate){
 requests.push_back({line,gate});return (int)requests.size()!=rejectAt;
}
void epd_set_mode(int m){modeState=m;}
void epd_lcd_frame_done_cb(frame_done_func_t,void*){}
void epd_lcd_line_source_cb(void*,void*){}
void epd_lcd_abort_frame(){}
void handle_lcd_frame_done(void*){}
void prepare_context_for_next_frame(RenderContext_t* c){
 c->frame_time=c->phase_times?c->phase_times[c->current_frame]:120;
}
int xPortGetCoreID(){return 0;}
void xTaskNotifyGive(int){++feeds;}
int xSemaphoreTake(int,int){if(failWait){--failWait;return 0;}return 1;}
void lq_reset(int*){}
''' + function(LCD / 'render_lcd.c', 'void lcd_do_update(') + r'''
int main(){
 RenderContext_t c;
 lcd_do_update(&c);assert(!c.error && !modeState && feeds==4);
 assert(requests.size()==3);
 for(auto r:requests)assert(r==std::make_pair(0,0)); // I2S 120 is not sent to LCD.
 requests.clear();feeds=0;int times[]{170,170};c={};c.phase_times=times;c.mode=10;
 lcd_do_update(&c);assert(!c.error && feeds==4 && !modeState);
 assert(requests[0]==std::make_pair(170,0));
 assert(requests.back()==std::make_pair(0,0));
 // Rejection before feeding and timeout both restore defaults and disable mode.
 requests.clear();feeds=0;rejectAt=1;c={};
 lcd_do_update(&c);assert(c.error && feeds==0 && !modeState);
 assert(requests.back()==std::make_pair(0,0));
 requests.clear();rejectAt=-1;failWait=1;c={};
 lcd_do_update(&c);assert(c.error && !modeState && requests.back()==std::make_pair(0,0));
}
'''
run(render, ['-Wno-unused-variable', '-DRICKYOS_PLUS_GRAY_CALIBRATION=0'])
run(render, ['-Wno-unused-variable', '-DRICKYOS_PLUS_GRAY_CALIBRATION=1'])
print('Actual LCD timing: synchronized clocks, constraints, live rejection, tail flush, default restore, diagnostic isolation and timeout/rejection cleanup passed (ASan/UBSan)')
