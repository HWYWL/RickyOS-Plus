"""Actual expander writes and Plus timer driver, with deterministic task/timer/I2C fakes."""
from pathlib import Path
import subprocess,tempfile
c=Path('firmware/rickyos-plus-1.2/components').resolve()
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)
 headers={
 'esp_err.h':'#pragma once\nusing esp_err_t=int; constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=-2,ESP_ERR_INVALID_STATE=-3,ESP_ERR_INVALID_ARG=-4;\ninline const char* esp_err_to_name(int){return "fake";}\n',
 'esp_log.h':'#pragma once\n#define ESP_LOGI(...)\n#define ESP_LOGD(...)\n',
 'driver/i2c.h':'#pragma once\nusing i2c_port_t=int;\n',
 'freertos/FreeRTOS.h':'#pragma once\n#include <cstdint>\nconstexpr int pdTRUE=1,portMAX_DELAY=-1;\n',
 'freertos/semphr.h':'#pragma once\n#include <mutex>\nusing StaticSemaphore_t=std::mutex;using SemaphoreHandle_t=std::mutex*;\ninline auto xSemaphoreCreateMutexStatic(StaticSemaphore_t* m){return m;}\ninline int xSemaphoreTake(SemaphoreHandle_t m,int){m->lock();return 1;}\ninline void xSemaphoreGive(SemaphoreHandle_t m){m->unlock();}\n',
 'esp_timer.h':'''#pragma once
#include <cstdint>
#include "esp_err.h"
struct Timer{void(*callback)(void*)=nullptr;bool armed=false;uint64_t due=0;};
using esp_timer_handle_t=Timer*;
struct esp_timer_create_args_t{void(*callback)(void*)=nullptr;int dispatch_method;const char* name;};
constexpr int ESP_TIMER_TASK=0;
extern int64_t fakeNow;extern Timer fakeTimer;extern bool failStart,failCreate;
inline int64_t esp_timer_get_time(){return fakeNow;}
inline int esp_timer_create(const esp_timer_create_args_t* a,Timer** t){if(failCreate)return ESP_FAIL;fakeTimer.callback=a->callback;*t=&fakeTimer;return ESP_OK;}
inline int esp_timer_stop(Timer* t){t->armed=false;return ESP_OK;}
inline int esp_timer_start_once(Timer* t,uint64_t delay){if(failStart){failStart=false;return ESP_FAIL;}t->armed=true;t->due=fakeNow+delay;return ESP_OK;}
'''}
 for name,body in headers.items():
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(body)
 # Keep actual headers/bodies. Rename only C's local helper name conflicting with no fake SDK API.
 source=(c/'metalio_board/bus/pca9555.c').read_text()
 (p/'expander.cpp').write_text(source)
 program='''#include <cassert>
#include <cstring>
#include <mutex>
#include "esp_timer.h"
#include "plus_i2c_wire.h"
#include "metalio_i2c1.h"
extern "C" {
#include "pca9555.h"
}
#include "PlusHaptic.h"
int64_t fakeNow=0;Timer fakeTimer;bool failStart=false,failCreate=false;
std::mutex bus;bool failWrite=false,failLock=false;int writes=0;uint8_t regs[8]{};
bool metalio_i2c1_lock(int){if(failLock)return false;bus.lock();return true;}
void metalio_i2c1_unlock(){bus.unlock();}
int plus_i2c_write(uint8_t,uint8_t reg,const uint8_t* data,size_t n){assert(n==1);writes++;if(failWrite){failWrite=false;return ESP_FAIL;}regs[reg]=*data;return ESP_OK;}
int plus_i2c_read(uint8_t,uint8_t reg,uint8_t* data,size_t n){memcpy(data,regs+reg,n);return ESP_OK;}
void tick(int64_t time){fakeNow=time;if(fakeTimer.armed&&fakeTimer.due<=uint64_t(time)){fakeTimer.armed=false;fakeTimer.callback(nullptr);}}
int main(){
 assert(pca9555_set_motor(true)==ESP_ERR_INVALID_STATE);
 assert(pca9555_set_value(0,0x25,0)==ESP_OK); // TPS, panel on; motor off
 failCreate=true;assert(!plus_haptic::begin());failCreate=false;assert(plus_haptic::begin());
 int idleWrites=writes;plus_haptic::stop();plus_haptic::stop();assert(writes==idleWrites);
 plus_haptic::pulse();assert(regs[2]==0x2d&&fakeTimer.armed);
 auto deadline=fakeTimer.due;fakeNow=1000;plus_haptic::pulse();assert(fakeTimer.due==deadline);
 // Audio/display writer while pulse is active: preserve motor and other rails.
 pca9555_set_value(0,0x36,0);assert(regs[2]==0x3e);
 tick(35000);assert(regs[2]==0x36&&!fakeTimer.armed);
 // Old port snapshot with the motor bit set cannot restart it.
 pca9555_set_value(0,0x3e,0);assert(regs[2]==0x36);
 plus_haptic::pulse();plus_haptic::stop();assert(regs[2]==0x36);
 // Stale callback is harmless during a replacement pulse.
 plus_haptic::pulse();fakeNow=36000;fakeTimer.callback(nullptr);assert(regs[2]&8);tick(70000);assert(!(regs[2]&8));
 // Start/write failure stops immediately. OFF failure arms a retry.
 failStart=true;plus_haptic::pulse();assert(!(regs[2]&8));
 failWrite=true;plus_haptic::pulse();assert(!(regs[2]&8));
 plus_haptic::pulse();failWrite=true;tick(105000);assert(fakeTimer.armed&&regs[2]&8);
 plus_haptic::pulse();tick(125000);assert(!(regs[2]&8)&&!fakeTimer.armed);
 plus_haptic::pulse();failLock=true;tick(160000);assert(fakeTimer.armed);failLock=false;tick(180000);assert(!(regs[2]&8));
 // P1 output is untouched by motor ownership.
 pca9555_set_value(0,0xaa,1);assert(regs[3]==0xaa);
}
'''
 (p/'test.cpp').write_text(program)
 flags=['clang++','-std=c++20','-fsanitize=address,undefined','-g','-pthread','-I'+str(p),'-I'+str(c/'metalio_board/bus'),'-I'+str(c/'rickyos_application/port')]
 # C linkage for real expander object matches its ESP-IDF C compilation.
 (p/'expander.cpp').write_text('extern "C" {\n'+source+'\n}\n')
 subprocess.run(flags+[str(p/'test.cpp'),str(p/'expander.cpp'),str(c/'rickyos_application/port/PlusHaptic.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual Plus motor/timer passed: no extension, stale callback, shared P0 preservation, stale write, idle stop, startup/start/I2C failure and OFF retry.')
