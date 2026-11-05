"""Run the actual LCD/RMT init/deinit functions with reference-counted clocks.

Host checks cover ownership, repeated reconstruction, and shared clock users;
they cannot establish peripheral retention, electrical timing, or sleep current.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_battery_stability import method

PORT = Path(__file__).resolve().parents[1]
SOURCE = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board/display/output_lcd/lcd_driver.c'


class ClockLifecycleTest(unittest.TestCase):
    def test_real_init_deinit_preserve_shared_owners(self):
        source = SOURCE.read_text()
        production = '\n'.join(method(source, signature) for signature in (
            'static void init_ckv_rmt()', 'static void deinit_ckv_rmt()',
            'static esp_err_t init_lcd_peripheral()', 'static void deinit_lcd_peripheral()',
        ))
        # The hardware is replaced, not the production lifecycle. Record every
        # source acquisition/release and retain one unrelated owner's reference.
        code = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define CONFIG_IDF_TARGET_ESP32S31 1
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6,1,0)
#define PERIPH_RCC_ATOMIC()
#define PERIPH_RCC_ACQUIRE_ATOMIC(module, ref) for(int ref=busRefs++,once=1;once;once=0)
#define PERIPH_RCC_RELEASE_ATOMIC(module, ref) for(int ref=--busRefs,once=1;once;once=0)
#define ESP_RETURN_ON_ERROR(result, ...) do { if ((result)!=0) return (result); } while(0)
#define ESP_LOGI(...) ((void)0)
using esp_err_t=int;
using soc_module_clk_t=int;
constexpr int ESP_OK=0,RMT_CLK_SRC_DEFAULT=1,LCD_CLK_SRC_DEFAULT=2;
[[maybe_unused]] constexpr int LCD_CORE_CLK_SRC_DEFAULT=3;
constexpr int RMT_CKV_CHAN=0,RMT_IDLE_LEVEL_LOW=0,PIN_FUNC_GPIO=0,GPIO_MODE_OUTPUT=1;
constexpr int ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED=0,LCD_LL_MEM_LP_MODE_SHUT_DOWN=0;
constexpr int ESP_INTR_FLAG_IRAM=1,ESP_INTR_FLAG_INTRDISABLED=2,ESP_INTR_FLAG_SHARED=4,ESP_INTR_FLAG_LOWMED=8;
constexpr int LCD_LL_EVENT_VSYNC_END=1,LCD_LL_EVENT_TRANS_DONE=2;
struct Panel {int module=0,irq_id=0;} panel;
#define LCD_PERIPH_PANEL panel
int busRefs=0,liveInterrupts=0,acquires=0,releases=0,RMT=0,hal=0;
int interruptAttempts=0,failInterrupt=0;
bool rmtBus=false,lcdClock=false;
bool lcdSource=false,coreSource=false,coreDivider=false,rmtSource=false;
int rmtDivider=0;
struct {struct {int bus_width=8;struct {int ckv=40;}bus;}config;struct {void* dev=nullptr;}hal;
 uint32_t ckv_tick_hz=0;int vsync_intr=0,done_intr=0,dummy_bytes=1;}lcd;
struct {struct {int tx_sig=0;}channels[1];}soc_rmt_signals[1];
std::map<int,int> refs;
int esp_clk_tree_enable_src(int source,bool on){
 assert(source>=1 && source<=3);
 if(on){++refs[source];++acquires;}
 else {
  assert(refs[source]>0);
  assert(source==RMT_CLK_SRC_DEFAULT ? !rmtBus : !lcdClock);
  --refs[source];++releases;
 }
 return 0;
}
int esp_clk_tree_src_get_freq_hz(int source,int,uint32_t* hz){assert(refs[source]>0);*hz=80000000;return 0;}
void rmt_ll_enable_bus_clock(int,bool on){rmtBus=on;}
void rmt_ll_reset_register(int){assert(rmtBus);rmtSource=false;rmtDivider=0;}
void lcd_ll_enable_bus_clock(int,bool){}
void lcd_ll_enable_clock(void*,bool on){lcdClock=on;}
void lcd_ll_reset_register(int){lcdSource=false;coreSource=false;coreDivider=false;}
void lcd_ll_select_clk_src(int,int source){assert(source==LCD_CLK_SRC_DEFAULT && refs[source]>0);lcdSource=true;}
void core_select(int,int source){assert(source==LCD_CORE_CLK_SRC_DEFAULT);coreSource=true;}
void lcd_ll_set_core_clock_divider(int,int div,int numerator,int denominator){assert(coreSource && div==2 && !numerator && !denominator);coreDivider=true;}
void rmt_ll_set_group_clock_src(int*,int,int source,int a,int b,int c){assert(source==RMT_CLK_SRC_DEFAULT && refs[source]>0 && a==1 && b==1 && !c);rmtSource=true;}
void rmt_ll_tx_set_channel_clock_div(int*,int,uint32_t div){assert(rmtSource && div==8);rmtDivider=div;}
uint32_t lcd_ll_get_interrupt_status_reg(void*){return 0;}
void lcd_isr_vsync(void*){}
int esp_intr_alloc_intrstatus(int,int,uint32_t,int,void(*)(void*),void*,int* handle){
 if(++interruptAttempts==failInterrupt)return -1;
 *handle=++liveInterrupts;return 0;
}
void esp_intr_free(int){assert(liveInterrupts>0);--liveInterrupts;}
void ckv_rmt_build_signal(){assert(refs[RMT_CLK_SRC_DEFAULT]>0 && rmtBus);}
// Other register operations are deliberately inert: they are not a simulation
// of clocks, GPIO routing, LCD timings or the physical display.
#define STUB(name) template<class... T> void name(T...) {}
STUB(rmt_ll_mem_force_power_on) STUB(rmt_ll_enable_mem_access_nonfifo)
STUB(rmt_ll_enable_group_clock)
STUB(rmt_ll_tx_set_mem_blocks)
STUB(rmt_ll_tx_fix_idle_level) STUB(rmt_ll_tx_enable_carrier_modulation) STUB(rmt_ll_tx_enable_loop)
STUB(gpio_hal_func_sel) STUB(gpio_set_direction) STUB(esp_rom_gpio_connect_out_signal) STUB(gpio_reset_pin)
STUB(lcd_hal_init) STUB(lcd_ll_mem_set_low_power_mode) STUB(lcd_ll_mem_power_by_pmu)
STUB(lcd_ll_enable_trans_buffer) STUB(lcd_ll_fifo_reset) STUB(lcd_ll_reset)
STUB(lcd_ll_set_clock_idle_level) STUB(lcd_ll_set_pixel_clock_edge) STUB(lcd_ll_enable_rgb_mode)
STUB(lcd_ll_set_dma_read_stride) STUB(lcd_ll_set_data_wire_width) STUB(lcd_ll_set_phase_cycles)
STUB(lcd_ll_enable_output_hsync_in_porch_region) STUB(lcd_ll_reverse_wire_bit_order)
STUB(lcd_ll_enable_output_always_on) STUB(lcd_ll_set_idle_level) STUB(lcd_ll_set_blank_cycles)
STUB(lcd_ll_enable_auto_next_frame) STUB(lcd_ll_enable_interrupt) STUB(esp_intr_enable) STUB(esp_intr_disable)
#define EPD_LCD_PROBE_ALWAYS_ON 1
#if WITH_CORE_CLOCK
#define lcd_ll_select_core_clk_src core_select
#endif
'''
        code += production
        code += r'''
int main(){
 // In the shared-bus case another driver owns the bus and all clock parents.
 for(int shared:{0,1}){
  refs.clear();busRefs=shared;acquires=releases=0;
  for(int source:{1,2,3})refs[source]=shared;
  for(int failure:{1,2}){
   interruptAttempts=0;failInterrupt=failure;
   assert(init_lcd_peripheral()!=ESP_OK);deinit_lcd_peripheral();
   assert(busRefs==shared && liveInterrupts==0 && !lcdClock);
   for(int source:{1,2,3})assert(refs[source]==shared);
  }
  failInterrupt=0;
  for(int cycle=0;cycle<1000;++cycle){
   // After teardown the previous register programming is not a valid restore.
   lcdSource=rmtSource=false;rmtDivider=0;
   if(!shared)coreSource=coreDivider=false;
   else coreSource=coreDivider=true; // The other bus owner retains the common core setup.
   assert(init_lcd_peripheral()==ESP_OK);init_ckv_rmt();
   assert(refs[1]==shared+1 && refs[2]==shared+1);
   assert(refs[3]==shared+WITH_CORE_CLOCK);
   assert(busRefs==shared+1 && liveInterrupts==2 && lcdClock && rmtBus);
   assert(lcdSource && rmtSource && rmtDivider==8 && lcd.ckv_tick_hz==10000000);
   if(WITH_CORE_CLOCK)assert(coreSource && coreDivider);
   deinit_lcd_peripheral();deinit_ckv_rmt();
   for(int source:{1,2,3})assert(refs[source]==shared);
   assert(busRefs==shared && liveInterrupts==0 && !lcdClock && !rmtBus);
   assert(acquires==releases);
  }
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.cpp').write_text(code)
            # Both IDF LL API versions supported by this Plus source.
            for core in (0, 1):
                subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                f'-DWITH_CORE_CLOCK={core}', str(root / 'test.cpp'),
                                '-o', str(root / 'test')], check=True)
                subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
