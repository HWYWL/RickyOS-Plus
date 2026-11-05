"""Actual sample sequencer, scan-window setter and startup/deadline tail; mocked LCD I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LCD = ROOT / 'firmware/rickyos-plus-1.2/components/metalio_board/display/output_lcd'


def function(signature):
    source = (LCD / 'lcd_driver.c').read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


tail = function('int64_t IRAM_ATTR epd_lcd_start_frame_at(')
tail = tail[tail.index('    const int prefix_us ='):]
samples = '\n'.join(line for line in (LCD / 'timed_samples.c').read_text().splitlines()
                    if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define RENDER_METHOD_LCD 1
#define RICKYOS_PLUS_GRAY_CALIBRATION 1
#define IRAM_ATTR
#define BOUNCE_BUF_LINES 8
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portYIELD_FROM_ISR() ((void)0)
typedef int BaseType_t;
enum EpdDrawError {EPD_DRAW_SUCCESS=0,EPD_DRAW_INVALID_CROP=0x40,
 EPD_DRAW_NO_PHASES_AVAILABLE=0x100,EPD_DRAW_EMPTY_LINE_QUEUE=0x400};
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {
 EpdRect area;int display_width,display_height,lines_consumed,lines_total,frame_done;
 int error;uint8_t* static_line_buffer;uint8_t* feed_line_buffers[2];
}RenderContext_t;
typedef bool (*line_cb_func_t)(RenderContext_t*,uint8_t*);
typedef void (*frame_done_func_t)(RenderContext_t*);
static struct {
 int default_display_lines,display_lines,scan_first_line;
 bool frame_active;int64_t frame_completed_us,frame_t0_us;
 int ckv_period_us,default_ckv_high_us10;
 struct {struct {int stv;}bus;}config;
 struct {void* dev;}hal;
}lcd;
static line_cb_func_t source_cb;
static frame_done_func_t done_cb;
static RenderContext_t* source_ctx;
static RenderContext_t* done_ctx;
static int64_t now,critical_at,max_critical;
static int locked,mode_on,scans,aborts,fail_at,fail_window_at,window_calls,fail_timing;
static int sem;
static uint8_t latch[304],scratch[1216];
static struct {int first,rows;bool dark;int64_t at;} records[64];
static int64_t min(int64_t a,int64_t b){return a<b?a:b;}
static int64_t esp_timer_get_time(void){return now;}
static void esp_rom_delay_us(int n){assert(n>=0);now+=n;}
static void vTaskDelay(int n){assert(!locked);now+=n*1000;}
#define taskENTER_CRITICAL(p) do{assert(!locked);locked=1;critical_at=now;}while(0)
#define taskEXIT_CRITICAL(p) do{assert(locked);if(now-critical_at>max_critical)max_critical=now-critical_at;locked=0;}while(0)
static void gpio_set_level(int pin,int value){(void)pin;(void)value;}
static void start_ckv_cycles(int cycles){
 assert(cycles==lcd.scan_first_line+lcd.display_lines+5);
 if(lcd.scan_first_line)for(int x=0;x<304;++x)assert(latch[x]==0);
}
static void lcd_ll_start(void* dev){
 (void)dev;assert(scans<64);records[scans].first=lcd.scan_first_line;
 records[scans].rows=lcd.display_lines;records[scans].at=now;++scans;
}
static void epd_lcd_frame_done_cb(frame_done_func_t cb,void* ctx){done_cb=cb;done_ctx=ctx;}
static void epd_lcd_line_source_cb(line_cb_func_t cb,void* ctx){source_cb=cb;source_ctx=ctx;}
static void epd_set_mode(int mode){mode_on=mode;}
static bool epd_lcd_set_line_timing(int period,int gate){
 assert(period==0 && gate==0);return !fail_timing;
}
static void epd_lcd_abort_frame(void){
 ++aborts;lcd.frame_active=false;lcd.frame_completed_us=now;
}
static void xSemaphoreGiveFromISR(int s,int* awoken){(void)s;*awoken=0;sem=1;}
static int xSemaphoreTake(int s,int ticks){
 (void)s;if(!ticks){int old=sem;sem=0;return old;}
 assert(ticks==30 && source_cb && done_cb && lcd.frame_active);
 const bool fail=scans==fail_at;
 for(int r=0;r<lcd.display_lines;++r){
   uint8_t row[304];memset(row,0xCC,sizeof(row));
   source_cb(source_ctx,row);
   for(int x=0;x<1216;++x){
     const int action=(row[x/4]>>(6-2*(x%4)))&3;
     assert(action==0 || action==1); // No reverse, undefined or white drive.
     if(action){
       assert(x>=source_ctx->area.x && x<source_ctx->area.x+source_ctx->area.width);
       assert(r<source_ctx->area.height);records[scans-1].dark=true;
     }
   }
   memcpy(latch,row,sizeof(row));now+=17;
   if(fail && r==lcd.display_lines/2)return 0; // Partial DMA/scan failure.
 }
 for(int x=0;x<304;++x)assert(!latch[x]); // Prefix may only follow a HOLD latch.
 lcd.frame_active=false;lcd.frame_completed_us=now;
 done_cb(done_ctx);int result=sem;sem=0;return result;
}
'''
window = function('bool epd_lcd_set_scan_window(').replace(
    'if (first == 0 && lines == 0)',
    'if (++window_calls == fail_window_at) return false;\n    if (first == 0 && lines == 0)')
code += '\n' + window
code += r'''
static int64_t epd_lcd_start_frame_at(int64_t scan_at_us){
 lcd.frame_active=true;
 int initial_lines=lcd.display_lines;
''' + tail + r'''
static void epd_lcd_start_frame(void){(void)epd_lcd_start_frame_at(0);}
''' + samples + r'''
static void reset(RenderContext_t* ctx){
 memset(&lcd,0,sizeof(lcd));lcd.default_display_lines=lcd.display_lines=688;
 lcd.ckv_period_us=17;lcd.default_ckv_high_us10=159;
 *ctx=(RenderContext_t){.area={1,2,3,4},.display_width=1216,.display_height=684};
 ctx->feed_line_buffers[0]=scratch;
 source_cb=NULL;done_cb=NULL;mode_on=scans=aborts=fail_at=fail_window_at=window_calls=fail_timing=sem=locked=0;
 now=1000;max_critical=0;memset(latch,0xFF,sizeof(latch));memset(records,0,sizeof(records));
}
int main(void){
 RenderContext_t ctx;EpdRect cells[16];
 for(int i=0;i<16;++i){
   int left=42+i*600/16,right=42+(i+1)*600/16;
   cells[i]=(EpdRect){608,684-right+1,202,right-left-3};
 }
 reset(&ctx);assert(epd_timed_samples_lcd(&ctx,cells,16)==EPD_DRAW_SUCCESS);
 assert(scans==31 && !mode_on && !locked && !lcd.frame_active);
 assert(!ctx.static_line_buffer && ctx.area.x==1 && ctx.area.y==2);
 assert(!source_cb && !done_cb && lcd.scan_first_line==0 && lcd.display_lines==688);
 assert(!records[0].dark && records[0].first==0 && records[0].rows==688);
 for(int n=0;n<15;++n){
   const int code=14-n,active=1+n*2,neutral=active+1;
   assert(records[active].dark && !records[neutral].dark);
   assert(records[active].first==cells[code].y && records[neutral].first==cells[code].y);
   assert(records[active].rows>=cells[code].height+4);
   assert(records[neutral].at-records[active].at==sample_durations_us[code]);
 }
 assert(max_critical<12000); // Deadline waiting is OUTSIDE critical sections.
 // Fail at every actual scan, including the initial neutral and last neutral.
 for(int failure=1;failure<=31;++failure){
   reset(&ctx);fail_at=failure;
   assert(epd_timed_samples_lcd(&ctx,cells,16)!=EPD_DRAW_SUCCESS);
   assert(aborts==1 && scans==failure+1 && !mode_on && !locked && !lcd.frame_active);
   assert(records[scans-1].first==0 && !records[scans-1].dark);
   assert(!ctx.static_line_buffer && !source_cb && !done_cb);
   assert(lcd.scan_first_line==0 && lcd.display_lines==688);
 }
 reset(&ctx);fail_window_at=2;
 assert(epd_timed_samples_lcd(&ctx,cells,16)!=EPD_DRAW_SUCCESS && !mode_on && !ctx.static_line_buffer);
 reset(&ctx);fail_timing=1;
 assert(epd_timed_samples_lcd(&ctx,cells,16)!=EPD_DRAW_SUCCESS && !mode_on && scans==0);
 reset(&ctx);assert(epd_timed_samples_lcd(&ctx,NULL,16)==EPD_DRAW_INVALID_CROP && scans==0);
 assert(epd_timed_samples_lcd(&ctx,cells,15)==EPD_DRAW_INVALID_CROP && scans==0);
 cells[8].y=680;
 assert(epd_timed_samples_lcd(&ctx,cells,16)==EPD_DRAW_INVALID_CROP && scans==0);
 // Real window setter: live, bounds, alignment, maximum and restoration.
 reset(&ctx);lcd.frame_active=true;assert(!epd_lcd_set_scan_window(80,40));
 lcd.frame_active=false;assert(!epd_lcd_set_scan_window(-1,40));
 assert(!epd_lcd_set_scan_window(80,41));assert(!epd_lcd_set_scan_window(660,40));
 assert(epd_lcd_set_scan_window(80,40));assert(epd_lcd_set_scan_window(0,0));
 assert(lcd.display_lines==688 && lcd.scan_first_line==0);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-timed-neutral-') as folder:
    p = Path(folder)
    (p/'test.c').write_text(code)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
print('Actual timed samples/window/startup tail: 3..150ms measured deadlines, original CKV, HOLD-only prefixes/padding, 31 timeout points, recovery and bounds passed (ASan/UBSan)')
