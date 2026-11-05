"""Execute checked LCD rebuild and rollback, injecting partial resource failures."""
from pathlib import Path
import unittest

from test_battery_stability import method, run_cpp

PORT = Path(__file__).resolve().parents[1]
DISPLAY = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board/display'


class RestoreFailuresTest(unittest.TestCase):
    def test_partial_allocations_dma_errors_retry_and_idempotent_deinit(self):
        source = (DISPLAY / 'output_lcd/lcd_driver.c').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6,1,0)
#define CONFIG_IDF_TARGET_ESP32S31 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define LCD_GDMA_DESC_ALIGN 8
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
#define DMA_DESCRIPTOR_BUFFER_MAX_SIZE 4096
#define DMA_DESCRIPTOR_BUFFER_OWNER_DMA 1
#define GDMA_TRIG_PERIPH_LCD 1
#define GDMA_MAKE_TRIGGER(a,b) (a)
#define ESP_RETURN_ON_FALSE(value,error,...) do {if(!(value))return (error);}while(0)
#define ESP_RETURN_ON_ERROR(result,...) do {if((result)!=0)return -1;}while(0)
#define ESP_GOTO_ON_ERROR(result,label,...) do {if((result)!=0)goto label;}while(0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=-2;
const char* esp_err_to_name(int){return "injected";}
using gdma_channel_handle_t=void*;
using gdma_trigger_t=int;
struct gdma_channel_alloc_config_t {int intr_priority;};
struct gdma_strategy_config_t {bool eof_till_data_popped;};
struct gdma_transfer_config_t {int max_data_burst_size;bool access_ext_mem;};
struct gdma_tx_event_callbacks_t {bool(*on_trans_eof)(void*,void*,void*);};
bool lcd_rgb_panel_eof_handler(void*,void*,void*){return false;}
struct Node {
 struct {int suc_eof=0,size=0,length=0,owner=0;}dw0;
 uint8_t* buffer;Node* next;
};
using lcd_dma_desc_t=Node;
struct LcdEpdConfig_t {int pixel_clock=20000000;};
struct {
 LcdEpdConfig_t config;
 uint8_t* bounce_buffer[2]{};size_t bb_size=32;bool bb_behind_cache=false;
 Node* dma_nodes=nullptr;void* dma_chan=nullptr;
}lcd;
bool lcd_initialized=false,dma_connected=false;
int allocAt=0,failAlloc=0,dmaAt=0,failDma=0,failPeripheral=0,irqCount=0,clockRefs=0;
bool failGpio=false,gpio=false,rmt=false,stopped=false;
bool sourceCallback=false,doneCallback=false;
std::set<void*> allocations;
// Preserve C's implicit void* assignment semantics in this C++ host harness.
struct CAllocation {void* p;template<class T>operator T*()const{return static_cast<T*>(p);}};
CAllocation heap_caps_aligned_calloc(size_t a,size_t n,size_t size,int){
 if(++allocAt==failAlloc)return {nullptr};
 void* p=nullptr;assert(!posix_memalign(&p,a<sizeof(void*)?sizeof(void*):a,n*size));
 memset(p,0,n*size);assert(allocations.insert(p).second);return {p};
}
void heap_caps_free(void* p){assert(allocations.erase(p)==1);free(p);}
void esp_cache_msync(void*,size_t,int){}
void assign_lcd_parameters_from_config(const LcdEpdConfig_t* c,int,int){lcd.config=*c;}
void check_cache_configuration(){}
int init_lcd_peripheral(){
 assert(clockRefs==0 && irqCount==0);clockRefs=1;
 if(failPeripheral==1)return -1;
 irqCount=1;
 if(failPeripheral==2)return -1;
 irqCount=2;return 0;
}
void deinit_lcd_peripheral(){assert(clockRefs==1);clockRefs=irqCount=0;}
int init_bus_gpio(){gpio=true;return failGpio?-1:0;}
void deinit_bus_gpio(){assert(gpio);gpio=false;}
void init_ckv_rmt(){assert(!rmt);rmt=true;}
void deinit_ckv_rmt(){assert(rmt);rmt=false;}
void epd_lcd_set_pixel_clock_MHz(int){assert(rmt && clockRefs && lcd.dma_chan);}
void epd_lcd_line_source_cb(void*,void*){sourceCallback=false;}
void epd_lcd_frame_done_cb(void*,void*){doneCallback=false;}
int new_channel(gdma_channel_alloc_config_t*,void** out,void*){
 assert(!*out);if(++dmaAt==failDma)return -1;
 *out=malloc(1);allocations.insert(*out);return 0;
}
#define LCD_GDMA_NEW_CHANNEL new_channel
int gdma_connect(void* p,int){assert(p);return ++dmaAt==failDma?-1:0;}
int gdma_apply_strategy(void* p,gdma_strategy_config_t*){assert(p);return ++dmaAt==failDma?-1:0;}
int gdma_set_priority(void* p,unsigned){assert(p);return ++dmaAt==failDma?-1:0;}
int gdma_config_transfer(void* p,gdma_transfer_config_t*){assert(p);return ++dmaAt==failDma?-1:0;}
int gdma_register_tx_event_callbacks(void* p,gdma_tx_event_callbacks_t*,void*){assert(p);return ++dmaAt==failDma?-1:0;}
void gdma_stop(void* p){assert(p && !sourceCallback && !doneCallback);stopped=true;}
void gdma_reset(void* p){assert(p && stopped);}
void gdma_disconnect(void* p){assert(p && dma_connected);}
void gdma_del_channel(void* p){assert(p && allocations.erase(p)==1);free(p);stopped=false;}
'''
        for signature in (
            'static esp_err_t allocate_lcd_buffers()', 'static void free_lcd_buffers()',
            'static esp_err_t init_dma_trans_link()', 'void deinit_dma_trans_link()',
            'bool epd_lcd_init_checked(', 'void epd_lcd_deinit()',
        ):
            code += '\n' + method(source, signature) + '\n'
        code += r'''
void clean(){
 assert(allocations.empty() && !lcd_initialized && !dma_connected && !lcd.dma_chan);
 assert(!lcd.bounce_buffer[0] && !lcd.bounce_buffer[1] && !lcd.dma_nodes);
 assert(!gpio && !rmt && irqCount==0 && clockRefs==0 && !sourceCallback && !doneCallback);
}
void resetFaults(){allocAt=dmaAt=0;failAlloc=failDma=failPeripheral=0;failGpio=false;}
int main(){
 LcdEpdConfig_t config;
 // Each buffer/descriptor allocation, both interrupt setup boundaries,
 // each DMA failure (including connected channel), and partial GPIO setup.
 for(int failure=0;failure<11;++failure){
  clean();resetFaults();
  if(failure<3)failAlloc=failure+1;
  else if(failure<5)failPeripheral=failure-2;
  else if(failure<10)failDma=failure-4;
  else failGpio=true;
  assert(!epd_lcd_init_checked(&config,1216,684));clean();
  epd_lcd_deinit();clean(); // Failed setup must not free resources twice.
  resetFaults();assert(epd_lcd_init_checked(&config,1216,684));
  assert(lcd_initialized && allocations.size()==4 && clockRefs==1 && irqCount==2);
  const int before=allocAt;
  assert(!epd_lcd_init_checked(&config,1216,684) && allocAt==before); // No duplicate setup leak.
  sourceCallback=doneCallback=true;
  epd_lcd_deinit();clean();epd_lcd_deinit();clean();
 }
 for(int cycle=0;cycle<1000;++cycle){
  resetFaults();assert(epd_lcd_init_checked(&config,1216,684));
  sourceCallback=doneCallback=true;epd_lcd_deinit();clean();
 }
}
'''
        run_cpp(code)

    def test_board_restore_failure_stays_unavailable_and_retryable(self):
        source = (DISPLAY / 'epd_board_metalio_eink4_plus.c').read_text()
        run_cpp(r'''
#include <cassert>
#include <cstddef>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
bool s_lcd_bus_held=true,s_tps_ok=true,fail=true;
int releases=0,prepares=0,attempts=0;
const int k_epd_bus_pins[]={8,9,10};
void gpio_hold_dis(int){++releases;}
void epd_ctrl_gpios_out_low(){++prepares;}
bool epd_lcd_reinit(){++attempts;return !fail;}
''' + method(source, 'bool epd_board_metalio_eink4_plus_bus_restore_checked(') + r'''
int main(){
 assert(!epd_board_metalio_eink4_plus_bus_restore_checked());
 assert(s_lcd_bus_held && !s_tps_ok && releases==3 && attempts==1);
 fail=false;
 assert(epd_board_metalio_eink4_plus_bus_restore_checked());
 assert(!s_lcd_bus_held && releases==6 && attempts==2 && prepares==2);
 // TPS only becomes ready through its normal power-on sequence.
 assert(!s_tps_ok);
 assert(epd_board_metalio_eink4_plus_bus_restore_checked() && attempts==2);
}
''')


if __name__ == '__main__':
    unittest.main()
