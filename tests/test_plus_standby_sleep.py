"""Execute Plus's actual sleep backend and activity with bounded fake hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_battery_stability import method, run_cpp

PORT = Path(__file__).resolve().parents[1]
FW = PORT / 'firmware/rickyos-plus-1.2'
UP = FW / 'upstream'


class PlusSleepTest(unittest.TestCase):
    def test_board_wake_classification_cleanup_errors_and_races(self):
        common = r'''
#pragma once
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED 1
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
using esp_err_t=int;
using gpio_num_t=int;
constexpr int ESP_OK=0,GPIO_INTR_LOW_LEVEL=4,GPIO_INTR_NEGEDGE=2;
constexpr int IO_EXPANDER_INT_GPIO=2,TOUCH_INT_PIN=5,TOUCH_I2C_ADDR=0x38,BOOT_BUTTON_GPIO=61,BOOT_BUTTON_ACTIVE_LEVEL=0;
constexpr int FL_COOL_PIN=6,FL_WARM_PIN=7;
constexpr int ESP_SLEEP_WAKEUP_EXT1=1,ESP_SLEEP_WAKEUP_TIMER=2,ESP_EXT1_WAKEUP_ANY_LOW=0,RTC_GPIO_MODE_INPUT_ONLY=1;
struct gpio_io_config_t { bool ie=true,oe=false,slp_sel=false,pu=false,pd=false; };
inline uint32_t now=0,wokeAt=0;
inline uint32_t millis(){return now;}
inline uint32_t firstDisplayFault=0;
inline uint32_t epd_lcd_get_first_fault(){return firstDisplayFault;}
inline void delay(unsigned ms){now+=ms;}
inline bool locked=false,busy=false,pending=false,irqLow=false,touchLow=false,bootLow=false,lockFails=false;
inline bool rtcMux=false,rtcClock=false,rtcInput=false,rtcHeld=false,rtcPu=false,rtcPd=false;
inline bool sourceWake=false,timerWake=false,isrEnabled=true,validWake=true;
inline bool digitalPu=false,digitalPd=false;
inline uint64_t otherExt1=1ULL<<6;
inline bool lcdParked=false,lcdLatched=false;
inline int lcdParks=0,lcdRestores=0;
inline unsigned lcdRestoreMs=0;
inline bool displayRestoreFails=false,usbClock=false,usbPad=true;
inline int usbRevives=0;
inline uint32_t usbDisconnectedAt=0;

inline bool plugOnArm=false,keyOnArm=false,irqOnArm=false,hold=false,keyAfterWake=false,irqLowAfterWake=false;
inline bool ioOutput=false,sleepSel=false;
inline int intrType=GPIO_INTR_NEGEDGE,cause=ESP_SLEEP_WAKEUP_TIMER,readAt=0,failRead=0;
inline int vbus=0,sleeps=0,usbReads=0,failUsb=0,touchCount=0,touchReads=0,failTouch=0;
inline bool host=false,hostOnArm=false,touchOnArm=false;
inline bool usb_serial_jtag_is_connected(){return host || (hostOnArm && sourceWake);}
inline std::string fault;
inline std::vector<std::string> calls;
inline void usb_serial_jtag_ll_enable_bus_clock(bool on){assert(on && !locked && isrEnabled);usbClock=on;calls.emplace_back("usb_clock");}
inline void usb_serial_jtag_ll_phy_enable_pad(bool on){
 assert(usbClock && !locked);
 if(on)assert(uint32_t(now-usbDisconnectedAt)>=150);else usbDisconnectedAt=now;
 usbPad=on;calls.emplace_back(on?"usb_connect":"usb_disconnect");if(on)++usbRevives;
}
inline void usb_serial_jtag_ll_phy_set_defaults(){assert(usbClock && !usbPad);calls.emplace_back("usb_defaults");}
inline uint64_t timerUs=0;
inline uint8_t initialP1=0xff;
inline int call(const char* s){calls.emplace_back(s);return fault==s?-1:ESP_OK;}
inline const char* esp_err_to_name(int){return "fake";}
template<class... T> void logIgnore(T...){}
#define ESP_LOGI(...) logIgnore(__VA_ARGS__)
#define ESP_LOGW(...) logIgnore(__VA_ARGS__)
#define ESP_LOGE(...) logIgnore(__VA_ARGS__)
#define STANDBY_DIAG(...) logIgnore(__VA_ARGS__)
inline bool metalio_i2c1_lock(int){assert(!locked);if(lockFails)return false;locked=true;return true;}
inline void metalio_i2c1_unlock(){assert(locked);locked=false;}
inline int plus_i2c_begin(){return ESP_OK;}
// Boot-time charger configuration is not part of these sleep paths: the probe
// reports no charger, and any configuration write would fail the test.
inline int plus_i2c_probe(int){return -1;}
inline int plus_i2c_write(int,int,const uint8_t*,size_t){assert(false);return -1;}
inline int plus_i2c_read(int addr,int reg,uint8_t* out,size_t n){
 assert(locked);
 if(addr==0x38){
  assert(reg==0x02 && n==5);++touchReads;
  for(size_t i=0;i<n;++i)out[i]=0;
  out[0]=uint8_t(touchCount || (touchOnArm && sourceWake));
  return touchReads==failTouch?-1:ESP_OK;
 }
 assert(addr==0x6b && reg==0x1e && n==1); // Read-only status; no config/gauge access.
 ++usbReads;*out=uint8_t(vbus || (plugOnArm && sourceWake));
 return usbReads==failUsb?-1:ESP_OK;
}
inline bool epd_board_metalio_eink4_plus_safe_io_init(){return true;}
inline void epd_board_metalio_eink4_plus_tp_reset(){}
inline void epd_board_metalio_eink4_plus_bus_hold_low(){
 assert(!locked && !lcdParked && !isrEnabled && rtcMux && rtcInput && sourceWake && timerWake);
 lcdParked=true;++lcdParks;calls.emplace_back("park_lcd");
}
inline void epd_board_metalio_eink4_plus_bus_latch_for_sleep(){assert(lcdParked && !locked);lcdLatched=true;calls.emplace_back("latch_lcd");}
inline bool epd_board_metalio_eink4_plus_bus_restore_checked(){
 assert(lcdParked && lcdLatched && !locked && !rtcMux && !rtcHeld && !sourceWake && !timerWake);
 now+=lcdRestoreMs;
 lcdParked=false;lcdLatched=false;++lcdRestores;calls.emplace_back("restore_lcd");
 return !displayRestoreFails;
}
inline void epd_board_metalio_eink4_plus_pwr_key_pulse_train(){assert(false);}
inline bool epd_board_metalio_eink4_plus_ioexp_read_port(int port,uint8_t* out){
 assert(!locked); // The real board accessor locks internally; reject nested bus locks.
 if(!metalio_i2c1_lock(20))return false;
 if(port==0){*out=0;metalio_i2c1_unlock();return true;}
 assert(port==1);++readAt;*out=initialP1;
 if((keyOnArm && sourceWake) || (keyAfterWake && sleeps)) {
   if(hold || !sleeps || uint32_t(now-wokeAt)<50) *out &= ~0x80;
 }
 const bool ok=readAt!=failRead;
 metalio_i2c1_unlock();return ok;
}
inline bool metalio_touch_has_pending_edges(){return pending;}
inline int gpio_get_level(int pin){
 if(pin==2){assert(!rtcMux);return !(irqLow || (irqOnArm && sourceWake));}
 if(pin==5)return !touchLow;
 assert(pin==61);return !bootLow;
}
inline int gpio_get_io_config(int pin,gpio_io_config_t* out){assert(pin==2 && !rtcMux);out->oe=ioOutput;out->slp_sel=sleepSel;out->pu=digitalPu;out->pd=digitalPd;return call("config");}
inline int gpio_intr_disable(int pin){assert(pin==2);isrEnabled=false;return call("mask");}
inline int gpio_intr_enable(int pin){assert(pin==2 && !rtcMux && !rtcHeld);isrEnabled=true;return call("unmask");}
inline bool esp_sleep_is_valid_wakeup_gpio(int pin){assert(pin==2);return validWake;}
inline int rtc_gpio_init(int pin){assert(pin==2 && !isrEnabled && !rtcMux);rtcMux=true;rtcClock=true;return call("rtc_init");}
inline int rtc_gpio_set_direction(int pin,int mode){assert(pin==2 && mode==RTC_GPIO_MODE_INPUT_ONLY && rtcClock && rtcMux);rtcInput=true;return call("rtc_input");}
inline int rtc_gpio_pullup_en(int pin){assert(pin==2 && rtcClock);rtcPu=true;return call("rtc_pu_en");}
inline int rtc_gpio_pullup_dis(int pin){assert(pin==2 && rtcClock);rtcPu=false;return call("rtc_pu_dis");}
inline int rtc_gpio_pulldown_en(int pin){assert(pin==2 && rtcClock);rtcPd=true;return call("rtc_pd_en");}
inline int rtc_gpio_pulldown_dis(int pin){assert(pin==2 && rtcClock);rtcPd=false;return call("rtc_pd_dis");}
inline int rtc_gpio_get_level(int pin){assert(pin==2 && rtcMux && rtcInput && !isrEnabled);return !(irqLow || (irqOnArm && sourceWake));}
// Frontlight pads: latched low around the light sleep.
inline int frontlightHeld=0;
inline int gpio_hold_en(int pin){assert(pin==FL_COOL_PIN || pin==FL_WARM_PIN);frontlightHeld|=1<<(pin-FL_COOL_PIN);return ESP_OK;}
inline int gpio_hold_dis(int pin){assert(pin==FL_COOL_PIN || pin==FL_WARM_PIN);frontlightHeld&=~(1<<(pin-FL_COOL_PIN));return ESP_OK;}
inline int rtc_gpio_hold_dis(int pin){assert(pin==2 && !sourceWake);rtcHeld=false;return call("rtc_unhold");}
inline int rtc_gpio_deinit(int pin){assert(pin==2 && !rtcHeld);rtcMux=false;rtcClock=false;rtcInput=false;rtcPu=false;rtcPd=false;return call("rtc_deinit");}
inline int esp_sleep_enable_ext1_wakeup_io(uint64_t mask,int mode){
 assert(mask==(1ULL<<2) && mode==ESP_EXT1_WAKEUP_ANY_LOW && rtcMux && rtcInput && !isrEnabled);
 sourceWake=true;return call("arm_source");
}
inline int esp_sleep_disable_ext1_wakeup_io(uint64_t mask){
 assert(mask==(1ULL<<2) && otherExt1==(1ULL<<6));sourceWake=false;return call("disable_source");
}
inline int esp_sleep_enable_timer_wakeup(uint64_t us){timerWake=true;timerUs=us;return call("arm_timer");}
inline int esp_sleep_disable_wakeup_source(int source){
 assert(source==ESP_SLEEP_WAKEUP_TIMER);timerWake=false;return call("disable_timer");
}
inline int esp_light_sleep_start(){
 assert(!locked && !isrEnabled && rtcMux && rtcClock && rtcInput && sourceWake && timerWake && intrType==GPIO_INTR_NEGEDGE && lcdParked && lcdLatched);
 assert(rtcPu==digitalPu && rtcPd==digitalPd);
 assert(timerUs>=1000000 && timerUs<=60000000);
 rtcHeld=true; // Actual IDF EXT1 prepare may hold the RTC pad, even on rejected sleep.
 ++sleeps;now+=1000;wokeAt=now;if(irqLowAfterWake)irqLow=true;return call("sleep");
}
inline int esp_sleep_get_wakeup_cause(){return cause;}
#define MALLOC_CAP_SPIRAM 0x400
inline void* heap_caps_malloc(size_t n,unsigned){return malloc(n);}
inline long long esp_timer_get_time(){return (long long)now*1000+1;}
inline unsigned esp_clk_cpu_freq(){return 360000000u;}
inline void mspi_timing_change_speed_mode_cache_safe(bool){}
namespace InputManager {
constexpr int BTN_POWER=0,BTN_UP=1,BTN_DOWN=2;
inline void setButtonHook(uint8_t(*)()){}
}
namespace freeink {struct PowerManager { static void setHostShutdownHook(void(*)()){} };}
namespace HalAudioOutput {using ReadCallback=int(*)(uint8_t*,size_t);}
namespace plus_audio {inline bool busy(){return ::busy;}inline void shutdown(){assert(false);}}
'''
        code = r'''
#include "fake.h"
#include "PRODUCTION"
using plus_board::StandbyWake;
void reset(){
 now=0;wokeAt=0;irqLowAfterWake=false;locked=false;busy=false;pending=false;irqLow=false;touchLow=false;bootLow=false;lockFails=false;
 rtcMux=false;rtcClock=false;rtcInput=false;rtcHeld=false;rtcPu=false;rtcPd=false;validWake=true;digitalPu=false;digitalPd=false;otherExt1=1ULL<<6;
 sourceWake=false;timerWake=false;isrEnabled=true;lcdParked=false;lcdLatched=false;lcdParks=0;lcdRestores=0;lcdRestoreMs=0;displayRestoreFails=false;usbClock=false;usbPad=true;usbRevives=0;usbDisconnectedAt=0;
 plugOnArm=false;keyOnArm=false;irqOnArm=false;hold=false;keyAfterWake=false;ioOutput=false;sleepSel=false;
 intrType=GPIO_INTR_NEGEDGE;cause=ESP_SLEEP_WAKEUP_TIMER;readAt=0;failRead=0;
 vbus=0;sleeps=0;usbReads=0;failUsb=0;touchCount=0;touchReads=0;failTouch=0;host=false;hostOnArm=false;touchOnArm=false;fault.clear();calls.clear();timerUs=0;initialP1=0xff;
 firstDisplayFault=0;plus_board::boardReady=false;plus_board::sleepFault=false;
 assert(!plus_board::standbySleepReady());assert(plus_board::begin());
 assert(plus_board::standbySleepReady());
}
void clean(){assert(!locked && !rtcMux && !rtcClock && !rtcInput && !rtcHeld && !rtcPu && !rtcPd && !sourceWake && !timerWake && isrEnabled && intrType==GPIO_INTR_NEGEDGE && !lcdParked && !lcdLatched && lcdParks==sleeps && lcdRestores==lcdParks && usbPad && usbRevives==sleeps && otherExt1==(1ULL<<6));}
int main(){
 reset();assert(plus_board::standbyLightSleep(12)==StandbyWake::Timer);assert(sleeps==1 && timerUs==12000000);clean();
 assert(plus_board::idleCpuFrequencyAllowed());
 busy=true;assert(!plus_board::idleCpuFrequencyAllowed());busy=false;
 // Reproduce the real first timer return followed by a failed redraw. No
 // second sleep or LCD teardown may start; the failure remains latched.
 firstDisplayFault=0x01aa0001;
 assert(!plus_board::standbySleepReady());
 assert(plus_board::standbyLightSleep(12)==StandbyWake::Failed && sleeps==1);clean();
 firstDisplayFault=0;
 assert(!plus_board::standbySleepReady());
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;
 assert(plus_board::standbyLightSleep(60)==StandbyWake::PowerButton);assert(sleeps==1);clean();
 // A real tap released during a slow LCD restore must not be lost.
 for(unsigned ms:{100u,1000u}){
  reset();cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;lcdRestoreMs=ms;
  assert(plus_board::standbyLightSleep(60)==StandbyWake::PowerButton);assert(now-wokeAt>=ms);clean();
  reset();keyAfterWake=true;lcdRestoreMs=ms; // Simultaneous timer and confirmed key.
  assert(plus_board::standbyLightSleep(1)==StandbyWake::PowerButton);clean();
  reset();cause=ESP_SLEEP_WAKEUP_EXT1;lcdRestoreMs=ms;
  assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed);clean(); // No invented key.
  reset();cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;lcdRestoreMs=ms;failRead=3;
  assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && lcdRestores==1);clean();
 }
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;hold=true;
 assert(plus_board::standbyLightSleep(1)==StandbyWake::PowerButtonHeld);assert(now-wokeAt==400);clean();
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;
 assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed);assert(sleeps==1);clean(); // Shared/ambiguous pulse.
 reset();displayRestoreFails=true;
 assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && usbRevives==1);clean();
 assert(!plus_board::standbySleepReady());
 displayRestoreFails=false;
 assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==1); // Fault stays latched.
 auto usbAt=calls.end(),lcdAt=calls.end();
 for(auto it=calls.begin();it!=calls.end();++it){if(*it=="usb_connect")usbAt=it;if(*it=="restore_lcd")lcdAt=it;}
 assert(usbAt<lcdAt); // USB survives a failed LCD rebuild, not blocked behind it.
 reset();keyAfterWake=true; // Key takes precedence over simultaneous timer wake.
 assert(plus_board::standbyLightSleep(1)==StandbyWake::PowerButton);clean();
 for(int n:{0,61}){reset();assert(plus_board::standbyLightSleep(n)==StandbyWake::Failed && sleeps==0);clean();}
 for(int state=0;state<7;++state){
  reset();switch(state){case 0:busy=true;break;case 1:pending=true;break;case 2:irqLow=true;break;
   case 3:touchCount=1;break;case 4:bootLow=true;break;case 5:initialP1=0x7f;break;default:ioOutput=true;}
  assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();
 }
 for(int usb=1;usb<=31;++usb){reset();vbus=usb;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();}
 reset();touchLow=true;assert(plus_board::standbyLightSleep(1)==StandbyWake::Timer && sleeps==1);clean(); // IRQ low is not contact truth.
 reset();host=true;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0 && usbReads==0);clean();
 for(int count:{2,3,15}){reset();touchCount=count;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();}
 for(int n:{1,2}){reset();failTouch=n;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();}
 reset();lockFails=true;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();
 for(int n:{1,2}){reset();failUsb=n;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();}
 for(int n:{1,2,3}){reset();failRead=n;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed);assert(sleeps==(n==3));clean();}
 reset();fault="sleep";
 assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && usbRevives==1 && lcdRestores==1);clean();
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;failRead=4;
 assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==1);clean(); // I2C error != release.
 for(const char* where:{"config","rtc_init","rtc_input","rtc_pu_dis","rtc_pd_dis","arm_source","arm_timer","sleep"}){
  reset();fault=where;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed);clean();
 }
 for(int race=0;race<5;++race){
  reset();if(race==0)plugOnArm=true;else if(race==1)irqOnArm=true;else if(race==2)keyOnArm=true;else if(race==3)hostOnArm=true;else touchOnArm=true;
  auto result=plus_board::standbyLightSleep(1);
  assert(sleeps==0 && result==(race==2?StandbyWake::PowerButton:StandbyWake::Failed));clean();
 }
 reset();initialP1=0xfd;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();
 for(const char* where:{"rtc_unhold","rtc_deinit","disable_source","disable_timer","unmask"}){
  reset();fault=where;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed);clean();
  assert(!plus_board::standbySleepReady());fault.clear();const auto count=sleeps;
  assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==count); // Sticky fault, no wake storm.
 }
 reset();now=UINT32_MAX-1030;cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;
 assert(plus_board::standbyLightSleep(1)==StandbyWake::PowerButton);clean();
 reset();validWake=false;assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0 && calls.size()==1);clean();
 for(bool pu:{false,true})for(bool pd:{false,true})for(bool selected:{false,true}){
  reset();digitalPu=pu;digitalPd=pd;sleepSel=selected;
  for(int n=0;n<125;++n){
   assert(plus_board::standbyLightSleep(1)==StandbyWake::Timer);clean();
   assert(digitalPu==pu && digitalPd==pd && sleepSel==selected);
  }
 }
 for(const char* where:{"rtc_pu_en","rtc_pd_en"}){
  reset();digitalPu=true;digitalPd=true;fault=where;
  assert(plus_board::standbyLightSleep(1)==StandbyWake::Failed && sleeps==0);clean();
 }
}
'''.replace('PRODUCTION', str(FW / 'components/rickyos_application/port/PlusBoard.cpp'))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'fake.h').write_text(common)
            for name in ['InputManager.h', 'PowerManager.h', 'plus_i2c_wire.h', 'metalio_i2c1.h',
                         'epd_board_metalio_eink4_plus.h', 'config.h', 'esp_log.h', 'Arduino.h', 'lcd_fault.h',
                         'driver/gpio.h', 'driver/rtc_io.h', 'driver/usb_serial_jtag.h', 'hal/usb_serial_jtag_ll.h', 'esp_sleep.h', 'metalio_touch.h', 'HalAudioOutput.h', 'StandbyDiag.h', 'esp_heap_caps.h', 'esp_timer.h', 'esp_private/esp_clk.h', 'esp_private/mspi_timing_tuning.h']:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('#include "fake.h"\n')
            source = root / 'test.cpp'
            binary = root / 'test'
            disabled = r'''
#include "fake.h"
#include "PRODUCTION"
int main(){
 assert(plus_board::begin());
 assert(!plus_board::standbySleepReady());
 assert(plus_board::standbyLightSleep(1)==plus_board::StandbyWake::Failed);
 assert(sleeps==0 && lcdParks==0 && lcdRestores==0 && usbReads==0 && touchReads==0 && calls.empty());
}
'''.replace('PRODUCTION', str(FW / 'components/rickyos_application/port/PlusBoard.cpp'))
            for experimental in (False, True):
                source.write_text(code if experimental else disabled)
                for diagnostic in (False, True):
                    definitions = ['-DRICKYOS_STANDBY_DIAG=1'] if diagnostic else []
                    if experimental:
                        definitions.append('-DRICKYOS_PLUS_STANDBY_SLEEP=1')
                    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                    *definitions, '-I' + directory, str(source), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)

    def test_shutdown_retries_oem_pulses_without_returning_to_idle(self):
        source = (FW / 'components/rickyos_application/port/PlusBoard.cpp').read_text()
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <vector>
#define ESP_LOGW(...) ((void)0)
namespace plus_audio {
int stops=0;
void shutdown(){++stops;}
}
int trains=0,waits=0;uint32_t now=0;
struct PowerCut {};
void delay(unsigned ms){assert(ms==200);++waits;now+=ms;}
void epd_board_metalio_eink4_plus_pwr_key_pulse_train(){
 assert(plus_audio::stops==1 && waits==trains);
 now+=3000; // OEM 15 high/low pulses, 100 ms each half.
 if(++trains==3)throw PowerCut{}; // Host stand-in for loss of execution on power cut.
}
namespace plus_board {
''' + method(source, 'void shutdown()') + r'''
}
int main(){
 try {plus_board::shutdown();assert(false && "must not return while powered");}
 catch(const PowerCut&){assert(trains==3 && waits==2 && now==9400 && plus_audio::stops==1);}
}
''')

    def test_hal_adapter_blocks_unfinished_side_key(self):
        source = (UP / 'lib/hal/HalPowerManager.cpp').read_text()
        run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#define FREEINK_DEVICE_METALIO_PLUS 1
#define FREEINK_DEVICE_READPICO 0
struct HalGPIO{};
std::atomic<bool> g_sideKeyBusy{false};
namespace plus_board {
enum class StandbyWake : uint8_t {Timer,PowerButton,PowerButtonHeld,Failed};
bool ready=true;unsigned calls=0;StandbyWake result=StandbyWake::Timer;
bool standbySleepReady(){return ready;}
StandbyWake standbyLightSleep(uint32_t){++calls;return result;}
}
struct HalPowerManager {
enum class LightSleepWakeReason {Timer,PowerButton,PowerButtonHeld,Failed};
static void setSideKeyBusy(bool);static bool sideKeyBusy();
bool canStandbyLightSleep(const HalGPIO&)const;
LightSleepWakeReason lightSleepFor(uint32_t)const;
};
''' + method(source, 'void HalPowerManager::setSideKeyBusy(') +
            method(source, 'bool HalPowerManager::sideKeyBusy()') +
            method(source, 'bool HalPowerManager::canStandbyLightSleep(') +
            method(source, 'HalPowerManager::LightSleepWakeReason HalPowerManager::lightSleepFor(') + r'''
int main(){
 HalPowerManager h;HalGPIO gpio;
 assert(h.canStandbyLightSleep(gpio));h.setSideKeyBusy(true);
 assert(!h.canStandbyLightSleep(gpio));
 assert(h.lightSleepFor(1)==HalPowerManager::LightSleepWakeReason::Failed && plus_board::calls==0);
 h.setSideKeyBusy(false);plus_board::ready=false;assert(!h.canStandbyLightSleep(gpio));plus_board::ready=true;
 for(auto result:{plus_board::StandbyWake::Timer,plus_board::StandbyWake::PowerButton,
                 plus_board::StandbyWake::PowerButtonHeld,plus_board::StandbyWake::Failed}){
  plus_board::result=result;
  assert(static_cast<unsigned>(h.lightSleepFor(1))==static_cast<unsigned>(result));
 }
 assert(plus_board::calls==4);
}
''')

    def test_activity_guards_timer_key_ownership_and_retry(self):
        activity = (UP / 'src/activities/apps/standby/StandbyActivity.cpp').read_text()
        time = (UP / 'src/activities/apps/standby/StandbyTime.cpp').read_text()
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <algorithm>
#define FREEINK_DEVICE_METALIO_PLUS 1
#define CROSSPOINT_EMULATED 0
#define STANDBY_DIAG(...) ((void)0)
uint32_t now=40000;uint32_t millis(){return now;}
struct Settings {bool standbySwipeUnlock=false;} SETTINGS;
struct Light {bool on=false;bool isOn()const{return on;}} Frontlight;
namespace bleinput {bool running=false;bool isRunning(){return running;}}
namespace HalRecorder {bool running=false;bool active(){return running;}}
struct Wifi {int mode=0;int getMode(){return mode;}} WiFi;
constexpr int WIFI_MODE_NULL=0;
struct Gpio {bool usb=false;bool isUsbConnected(){return usb;}} gpio;
struct RenderLock {~RenderLock(){}};
struct HalPowerManager {
 enum class LightSleepWakeReason {Timer,PowerButton,PowerButtonHeld,Failed};
 bool allowed=true;int sleeps=0;uint32_t seconds=0;LightSleepWakeReason reason=LightSleepWakeReason::Timer;
 bool canStandbyLightSleep(const Gpio&)const{return allowed;}
 LightSleepWakeReason lightSleepFor(uint32_t n){++sleeps;seconds=n;return reason;}
} powerManager;
struct Manager {int closed=0;void closeStandby(){++closed;}} activityManager;
namespace standby_time {
constexpr unsigned kLightSleepIdleMs=35000;
bool shouldLightSleep(uint32_t,bool,bool,bool,bool);
}
''' + 'namespace standby_time {\n' + method(time, 'bool shouldLightSleep(') + r'''
}
constexpr uint32_t kMaxLightSleepSeconds=60;
struct Face {bool missing=false;uint32_t next=3600;
 bool needsPicture(){return missing;}uint32_t secondsUntilNextWake(){return next;}} face;
struct StandbyActivity {
 struct Renderer {void waitRefreshComplete(){}} renderer;
 enum class DisplayMode {Normal,Immersive};enum class SyncState {Idle,Syncing};
 DisplayMode mode_=DisplayMode::Immersive;SyncState syncState_=SyncState::Idle;
 Face* currentFace_=&face;uint32_t lastInputMs_=0;int ticks=0,updates=0;
 bool tryLightSleep(uint32_t);
 void processFaceTick(bool wait){assert(wait);++ticks;}void requestUpdate(){++updates;}
};
''' + method(activity, 'bool StandbyActivity::tryLightSleep(') + r'''
int main(){
 for(int guard=0;guard<10;++guard){
  StandbyActivity a;face={};SETTINGS={};Frontlight={};bleinput::running=false;
  HalRecorder::running=false;WiFi.mode=0;gpio.usb=false;powerManager={};
  unsigned idle=35000;
  switch(guard){case 0:SETTINGS.standbySwipeUnlock=true;break;case 1:bleinput::running=true;break;
   case 2:HalRecorder::running=true;break;case 3:Frontlight.on=true;break;case 4:WiFi.mode=1;break;
   case 5:gpio.usb=true;break;case 6:face.missing=true;break;case 7:idle=34999;break;
   case 8:powerManager.allowed=false;break;default:a.syncState_=StandbyActivity::SyncState::Syncing;}
  assert(!a.tryLightSleep(idle)&&powerManager.sleeps==0);
 }
 SETTINGS={};Frontlight={};bleinput::running=false;HalRecorder::running=false;WiFi.mode=0;gpio.usb=false;face={};
 for(auto reason:{HalPowerManager::LightSleepWakeReason::Timer,HalPowerManager::LightSleepWakeReason::PowerButton,
                 HalPowerManager::LightSleepWakeReason::PowerButtonHeld,HalPowerManager::LightSleepWakeReason::Failed}){
  StandbyActivity a;powerManager={};activityManager={};powerManager.reason=reason;
  assert(a.tryLightSleep(35000)&&powerManager.sleeps==1&&powerManager.seconds==60);
  assert(a.ticks==(reason==HalPowerManager::LightSleepWakeReason::Timer));
  assert(activityManager.closed==(reason==HalPowerManager::LightSleepWakeReason::PowerButton));
  assert(a.updates==(reason==HalPowerManager::LightSleepWakeReason::PowerButtonHeld));
  if(reason==HalPowerManager::LightSleepWakeReason::Failed){
   const auto last=a.lastInputMs_;now+=4999;assert(!a.tryLightSleep(uint32_t(now-last)));
   now+=1;assert(a.tryLightSleep(uint32_t(now-last))); // 5s retry; no busy loop.
  }
 }
}
''')


if __name__ == '__main__':
    unittest.main()
