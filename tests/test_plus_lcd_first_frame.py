"""Execute real PCLK programming, frame start, bounce fill and EOF completion.

Hardware registers/timer/DMA are collaborators; this is not an electrical model.
"""
from pathlib import Path
import subprocess
import tempfile

from test_battery_stability import method

PORT = Path(__file__).resolve().parents[1]
LCD = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board/display/output_lcd'
source = (LCD / 'lcd_driver.c').read_text()
struct_start = source.index('typedef struct {\n    lcd_hal_context_t hal;')
struct_end = source.index('} s3_lcd_t;', struct_start) + len('} s3_lcd_t;')

code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "lcd_driver.h"
static inline int min(int a,int b){return a<b?a:b;}
static inline int max(int a,int b){return a>b?a:b;}
#define CONFIG_IDF_TARGET_ESP32S31 1
#define IRAM_ATTR
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6,1,0)
#define LCD_HAL_PCLK_FLAG_ALLOW_EQUAL_SYSCLK 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define PERIPH_RCC_ATOMIC()
#define taskENTER_CRITICAL(p) (++locked)
#define taskEXIT_CRITICAL(p) (--locked)
#define BOUNCE_BUF_LINES 8
#define LINE_BATCH 1000
#define RMT_CKV_CHAN 0
#define EPD_LCD_PROBE_BOUNCE 8
#define EPD_LCD_PROBE_ALWAYS_ON 1
#define EPD_LCD_PROBE_D0_TOGGLE 0
#define DMA_DESCRIPTOR_BUFFER_OWNER_DMA 1
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
using intr_handle_t=void*;using gdma_channel_handle_t=void*;
struct gdma_event_data_t{};
struct lcd_dma_desc_t {struct {int owner=0;}dw0;};
struct Clock {int clk_en=0,lcd_clk_sel=0,lcd_clkm_div_num=0,lcd_clkm_div_a=0,lcd_clkm_div_b=0,lcd_clkcnt_n=0,lcd_clk_equ_sysclk=0;};
struct Device {Clock lcd_clock;}device;
struct lcd_hal_context_t{Device* dev=&device;};
struct {struct {int reg_lcd_clk_en=0,reg_lcd_clk_div_num=0;}lcdcam_lcd_ctrl0;}HP_SYS_CLKRST;
struct hal_utils_clk_div_t{uint32_t integer=0,numerator=0,denominator=0;};
struct rmt_item32_t{int duration0=0,level0=0,duration1=0,level1=0,val=0;};
struct {struct {rmt_item32_t data32[2];}chan[1];}RMTMEM;
'''
code += source[struct_start:struct_end]
code += r'''
static s3_lcd_t lcd{};
bool border_whitening=false,dmaRunning=false,lcdRunning=false;
int locked=0,sourceRow=0,done=0,startCalls=0,fifoResets=0;
int64_t now=1000000;
uint32_t hpDivider=0;
std::vector<int> events;
int64_t esp_timer_get_time(){return ++now;}
void esp_rom_delay_us(int us){assert(us>=0);now+=us;}
void vTaskDelay(int ticks){now+=ticks*1000;}
void check_cache_configuration(){}
uint32_t lcd_hal_cal_pclk_freq(lcd_hal_context_t*,uint32_t src,uint32_t requested,int,hal_utils_clk_div_t* div){
 assert(src==160000000 && requested>0);div->integer=src/requested;return src/div->integer;
}
void lcd_ll_set_group_clock_coeff(int,int integer,int,int){hpDivider=integer;events.push_back(1);}
void lcd_ll_set_horizontal_timing(Device*,int a,int b,int c,int d){assert(a+b+c+d==lcd.line_cycles);}
void lcd_ll_set_vertical_timing(Device*,int,int,int active,int){assert(active==lcd.display_lines);}
void lcd_ll_set_hsync_position(Device*,int){}
void lcd_ll_stop(Device*){lcdRunning=false;events.push_back(3);}
void gdma_reset(void*){dmaRunning=false;events.push_back(2);}
void gdma_stop(void*){dmaRunning=false;}
void lcd_ll_fifo_reset(Device*){assert(!lcdRunning);++fifoResets;events.push_back(4);}
void lcd_ll_enable_auto_next_frame(Device*,bool on){assert(on);}
void gdma_start(void*,intptr_t address){
 assert(!lcdRunning && !dmaRunning && address==(intptr_t)&lcd.dma_nodes[0]);
 assert(sourceRow==16 && lcd.lines_fed==16 && !lcd.frame_finishing && lcd.bb_eof_count==0);
 assert(lcd.dma_nodes[0].dw0.owner==1 && lcd.dma_nodes[1].dw0.owner==1);
 dmaRunning=true;events.push_back(5);
}
void start_ckv_cycles(int cycles){assert(locked==1 && dmaRunning && cycles==lcd.display_lines+5);events.push_back(6);}
void gpio_set_level(int,int){}
void lcd_ll_start(Device*){
 assert(locked==1 && dmaRunning && !lcdRunning && sourceRow==16 && lcd.frame_t0_us>0);
 assert(device.lcd_clock.clk_en==1 && device.lcd_clock.lcd_clk_sel==2);
 assert(device.lcd_clock.lcd_clkm_div_num==1 && hpDivider>=1);
 lcdRunning=true;++startCalls;events.push_back(7);
}
void log_lcd_suspects(){}
void esp_cache_msync(void*,size_t,int){}
void epd_filter_white_border(uint8_t*,int,int,int,int,bool){}
bool row_source(void*,uint8_t* row){
 memset(row,sourceRow<lcd.native_height?(sourceRow%251)+1:0,lcd.line_bytes);++sourceRow;return false;
}
void frame_done(void*){assert(!dmaRunning && !lcdRunning && !lcd.frame_active);++done;}
struct EpdLcdRefillStats {uint32_t stale,refills;int32_t max_dt_us,max_work_us,worst_line,period_us;};
static EpdLcdRefillStats s_refill;static int64_t s_refill_worst;
'''
for signature in (
    'static void ckv_rmt_build_signal()',
    'static void assign_lcd_parameters_from_config(',
    'void epd_lcd_set_pixel_clock_MHz(',
    'static IRAM_ATTR void lcd_finish_frame_from_isr(',
    'static IRAM_ATTR bool fill_bounce_buffer(',
    'static IRAM_ATTR bool lcd_rgb_panel_eof_handler(',
    'int64_t IRAM_ATTR epd_lcd_start_frame_at(',
):
    code += '\n' + method(source, signature) + '\n'
code += r'''
int main(){
 std::array<uint8_t,2440> a{},b{};lcd_dma_desc_t nodes[2];
 LcdEpdConfig_t config{};config.bus_width=8;config.le_high_time=9;
 config.line_front_porch=4;config.dummy_clocks_after_data=10;config.ckv_high_time=60;
 for(int cycle=0;cycle<1000;++cycle){
  // Registers lose the previous configuration on reconstruction; software may
  // still contain the previous frame's counters. Do not zero the lcd object.
  device={};HP_SYS_CLKRST={};hpDivider=0;
  lcd.lines_fed=688;lcd.bb_eof_count=86;lcd.batches=10;lcd.frame_finishing=true;
  lcd.frame_active=false;lcd.frame_t0_us=1;lcd.eof_dt_n=1;lcd.eof_dt_sum_us=123;
  lcd.line_cycles=999;lcd.ckv_period_ticks=999;lcd.config.ckv_high_time=999;
  lcd.default_line_cycles=999;lcd.default_ckv_high_us10=999;
  now+=60000000;sourceRow=done=startCalls=fifoResets=0;events.clear();
  int mhz=cycle%3==0?20:cycle%3==1?16:10;config.pixel_clock=mhz*1000000;
  assign_lcd_parameters_from_config(&config,1216,684);
  lcd.ckv_tick_hz=10000000; // The separate real RMT init test verifies this divider.
  epd_lcd_set_pixel_clock_MHz(mhz);
  assert(hpDivider==(uint32_t)(160/mhz) && lcd.config.pixel_clock==(uint32_t)mhz*1000000);
  assert(!lcd.ckv_period_ticks && lcd.default_line_cycles==lcd.line_cycles);
  assert(lcd.default_ckv_high_us10==lcd.config.ckv_high_time);
  const int cycles=lcd.line_cycles;
  assert(RMTMEM.chan[0].data32[0].duration0+RMTMEM.chan[0].data32[0].duration1==lcd.ckv_period_us*10);
  lcd.bounce_buffer[0]=a.data();lcd.bounce_buffer[1]=b.data();lcd.dma_nodes=nodes;
  lcd.line_source_cb=row_source;lcd.frame_done_cb=frame_done;
  const int64_t deadline=cycle%2?now+5000:0;
  const int64_t started=epd_lcd_start_frame_at(deadline);
  assert(!deadline || started>=deadline);
  assert(locked==0 && done==0 && startCalls==1 && fifoResets==1 && lcd.line_cycles==cycles);
  assert(events==std::vector<int>({1,2,3,4,5,6,7}));
  int sent=0;
  while(!lcd.frame_finishing){
   int bb=lcd.bb_eof_count%2;
   for(int row=0;row<8;++row){
    const uint8_t expected=sent<684?sent%251+1:0;
    for(int col=0;col<lcd.line_bytes;++col)
     assert(lcd.bounce_buffer[bb][row*(lcd.line_bytes+1)+1+col]==expected);
    ++sent;
   }
   lcd_rgb_panel_eof_handler(nullptr,nullptr,nullptr);
  }
  assert(sent==688 && sourceRow==688 && done==1 && !lcd.frame_active && !lcdRunning && !dmaRunning);
  lcd_rgb_panel_eof_handler(nullptr,nullptr,nullptr);assert(done==1);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='plus-first-frame-') as directory:
    root = Path(directory)
    (root / 'driver').mkdir()
    (root / 'driver/gpio.h').write_text('#pragma once\ntypedef int gpio_num_t;\n')
    (root / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                    '-fsanitize=address,undefined', '-I', str(root), '-I', str(LCD),
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Actual PCLK programming + first-frame startup: 1000 stale-state cycles, FIFO reset, 16-row prefill, DMA-before-LCD, deadlines and all 688 rows passed (ASan/UBSan)')
