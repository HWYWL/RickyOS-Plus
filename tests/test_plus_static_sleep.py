"""Run the static standby storage lease against file/mount failures, using production code."""
from pathlib import Path
import ast
import subprocess
import tempfile
import unittest
from test_battery_stability import method, run_cpp

PORT = Path(__file__).resolve().parents[1]
UP = PORT / 'firmware/rickyos-plus-1.2/upstream'

class StaticStandbyTest(unittest.TestCase):
    def test_real_rail_restore_separates_load_steps_and_reports_errors(self):
        source=(PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display/epd_board_metalio_eink4_plus.c').read_text()
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <vector>
using esp_err_t=int;constexpr int ESP_OK=0,CFG_PORT_INT=1,CFG_PORT_CTRL=0;
constexpr uint8_t CFG_PIN_BT_PA_PWR=0x10,CFG_PIN_CAM_SCR=0x20;
bool s_ioexp_ok=true,s_sleep_keys_restored=true;uint8_t s_p1_out=4,s_cam_scr_on=0;int failPort=-1;unsigned now=0;
bool preflightOk=true;bool epd_board_metalio_eink4_plus_sleep_io_checked(bool){return preflightOk;}
struct {int port=1;} config_reg;
struct Write{int port;uint8_t value;unsigned when;};std::vector<Write> writes;std::vector<unsigned> phases;
uint8_t p0_output_value(){return s_cam_scr_on;}
int pca9555_set_value(int bus,uint8_t value,int port){assert(bus==1);writes.push_back({port,value,now});return port==failPort?-1:ESP_OK;}
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms){assert(ms==40);now+=ms;}
void trace(unsigned phase){phases.push_back(phase);}
''' + method(source,'bool epd_board_metalio_eink4_plus_sleep_restore_rails_staged(') + r'''
int main(){
 for(int failure:{-1,0,1}){
  writes.clear();phases.clear();s_p1_out=4;s_cam_scr_on=0;now=0;failPort=failure;
  assert(epd_board_metalio_eink4_plus_sleep_restore_rails_staged(true,trace)==(failure==-1));
  assert(writes.size()==2 && writes[0].port==1 && writes[0].value==0x14 && writes[0].when==0);
  assert(writes[1].port==0 && writes[1].value==0x20 && writes[1].when>=40 && now>=80);
  assert((phases==std::vector<unsigned>{0,1,2,3,4,5}));
 }
 s_ioexp_ok=false;writes.clear();phases.clear();assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(true,trace));
 assert(writes.empty() && phases.empty());
 s_ioexp_ok=true;s_sleep_keys_restored=true;failPort=-1;assert(epd_board_metalio_eink4_plus_sleep_restore_rails_staged(true,nullptr));
 writes.clear();phases.clear();assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(false,trace));
 assert(writes.empty() && phases.empty());s_sleep_keys_restored=true;preflightOk=false;
 assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(true,trace));assert(writes.empty() && phases.empty());
}
''')

    def test_breadcrumb_survives_restart_and_interrupted_commit(self):
        header=PORT/'firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusSleepBreadcrumb.h'
        run_cpp('#include "'+str(header)+'"\n' + r'''
#include <cassert>
using namespace plus_sleep_trace;
volatile Record slots[2]{};
int main(){
 static_assert(sizeof(slots)==144);
 assert(!latest(slots).valid);
 checkpoint(slots,Stage::Boot,0);checkpoint(slots,Stage::SdStop,35000);
 checkpoint(slots,Stage::RailsOff,35200);checkpoint(slots,Stage::SleepEnter,35201);
 const auto before=latest(slots);assert(before.valid && before.record.words[3]==1 && before.record.words[4]==35201);
 // Restart: only RTC memory remains; no normal-RAM index or counter is needed.
 auto rebooted=latest(slots);assert(rebooted.record.words[6+rebooted.record.words[5]-1]==uint32_t(Stage::SleepEnter));
 checkpoint(slots,Stage::Boot,0);assert(latest(slots).record.words[3]==1);
 // Power/reset interrupting any word of the next commit must preserve the previous record.
 Record saved[2];for(unsigned a=0;a<2;++a)for(unsigned w=0;w<kWords;++w)saved[a].words[w]=slots[a].words[w];
 const auto good=latest(slots);
 for(unsigned prefix=0;prefix<kDigest;++prefix){
  for(unsigned a=0;a<2;++a)for(unsigned w=0;w<kWords;++w)slots[a].words[w]=saved[a].words[w];
  const unsigned target=good.index^1u;slots[target].words[kDigest]=0;
  for(unsigned w=0;w<prefix;++w)slots[target].words[w]=0xdeadbeef;
  assert(latest(slots).valid && latest(slots).record.words[2]==good.record.words[2]);
 }
 for(unsigned a=0;a<2;++a)for(unsigned w=0;w<kWords;++w)slots[a].words[w]=saved[a].words[w];
 for(unsigned n=0;n<100;++n)checkpoint(slots,Stage::SleepEnter,n);
 auto current=latest(slots);assert(current.valid && current.record.words[3]==101 && current.record.words[5]==8);
 checkpoint(slots,Stage::SleepReturned,200,4,0,true);
 assert(latest(slots).record.words[14]==4 && latest(slots).record.words[15]==0 && latest(slots).record.words[16]==1);
 checkpoint(slots,Stage::SleepReturned,300,1,0,false);
 assert(latest(slots).record.words[14]==1 && latest(slots).record.words[16]==1);
 checkpoint(slots,Stage::SleepReturned,400,4,uint32_t(-1),true);
 assert(latest(slots).record.words[15]==uint32_t(-1) && latest(slots).record.words[16]==1);
 current=latest(slots);
 // A corrupted newest snapshot falls back to the other complete slot.
 slots[current.index].words[4]^=1;assert(latest(slots).record.words[2]==current.record.words[2]-1);
 // Sequence numbers remain ordered across wraparound.
 Record nearWrap=current.record;nearWrap.words[2]=0xffffffffu;nearWrap.words[kDigest]=digest(nearWrap);
 for(unsigned w=0;w<kWords;++w)slots[0].words[w]=nearWrap.words[w];slots[1].words[kDigest]=0;
 checkpoint(slots,Stage::SleepReturned,42);assert(latest(slots).record.words[2]==0 && latest(slots).record.words[4]==42);
}
''')

    def test_tca_register_pairs_and_real_io_state_check(self):
        source=(PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display/epd_board_metalio_eink4_plus.c').read_text()
        run_cpp(r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
using esp_err_t=int;constexpr int ESP_OK=0;
bool s_ioexp_ok=true,locked=false,failLock=false;
uint8_t registers[8]{},expectedP0=0,s_p1_out=0;
constexpr uint8_t CFG_P0_INPUT_MASK=0xc0,CFG_P1_INPUT_MASK=0xe3;
[[maybe_unused]] constexpr int EPD_IO_POWER=15;
#define CFG_IO_BIT(io) (1u << (unsigned(io)&7u))
constexpr uint8_t CFG_P1_SLEEP_WAKE_KEYS=0x80; // power key only
uint8_t p0_output_value(){return expectedP0;}
uint8_t pca9555_get_addr(){return 0x21;}
std::vector<int> commands;int failAt=-1;
bool metalio_i2c1_lock(int){assert(!locked);if(failLock)return false;locked=true;return true;}
void metalio_i2c1_unlock(){assert(locked);locked=false;}
int plus_i2c_read(uint8_t addr,uint8_t reg,uint8_t* out,size_t count){
 assert(locked && addr==0x21);commands.push_back(reg);
 // TI 9.5.2.1.2: repeated reads toggle within the selected register pair.
 for(size_t i=0;i<count;++i)out[i]=registers[(reg&~1)|((reg+i)&1)];
 return reg==failAt?-1:ESP_OK;
}
''' + method(source,'bool epd_board_metalio_eink4_plus_sleep_io_checked(') + r'''
void setup(bool parked){
 registers[2]=expectedP0=parked?0:0x20;registers[3]=s_p1_out=parked?0x87:0x14;
 registers[4]=registers[5]=0;registers[6]=parked?0:CFG_P0_INPUT_MASK;
 registers[7]=parked?0x80:CFG_P1_INPUT_MASK;
 commands.clear();
}
int main(){
 for(bool parked:{false,true}){
  setup(parked);assert(epd_board_metalio_eink4_plus_sleep_io_checked(parked) && !locked);
  assert((commands==std::vector<int>{2,4,6}));
  uint8_t legacy[6]{};metalio_i2c1_lock(20);plus_i2c_read(0x21,2,legacy,6);metalio_i2c1_unlock();
  assert(legacy[2]==expectedP0 && legacy[3]==s_p1_out && legacy[5]!=registers[7]); // reproduces static1 false failure
  for(int reg=2;reg<8;++reg){
   setup(parked);registers[reg]^=1;assert(!epd_board_metalio_eink4_plus_sleep_io_checked(parked) && !locked);
  }
  for(int reg:{2,4,6}){
   setup(parked);failAt=reg;assert(!epd_board_metalio_eink4_plus_sleep_io_checked(parked) && !locked);failAt=-1;
  }
 }
 setup(true);s_ioexp_ok=false;assert(!epd_board_metalio_eink4_plus_sleep_io_checked(true) && commands.empty());
 s_ioexp_ok=true;failLock=true;assert(!epd_board_metalio_eink4_plus_sleep_io_checked(true) && !locked && commands.empty());
}
''')

    def test_board_static_park_restore_order_and_failures(self):
        # Reuse the bounded RTC/USB/LCD fakes of the existing Plus sleep tests.
        tree = ast.parse((PORT/'tests/test_plus_standby_sleep.py').read_text())
        common = next(ast.literal_eval(node.value) for node in ast.walk(tree)
                      if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id=='common' for t in node.targets))
        # This test brings its own IMU write fake in place of the charger-write guard.
        common = common.replace('inline int plus_i2c_write(int,int,const uint8_t*,size_t){assert(false);return -1;}\n', '')
        common = common.replace('inline int plus_i2c_read(', r'''
constexpr int SC7A20H_I2C_ADDR_DEFAULT=0x19,SC7A20H_I2C_ADDR_ALT=0x18;
inline uint8_t imuRegs[256]{};
inline bool storageOff=false,sdHeld=false,keysMasked=false,railsOff=false;
inline bool sdFail=false,suspendFail=false,remountFail=false,parkCheckFail=false,restoreCheckFail=false,imuFail=false,keysRestoreFail=false;
inline int plus_i2c_write(int addr,int reg,const uint8_t* value,size_t n){
 assert(locked && addr==0x19 && n==1);imuRegs[reg]=*value;return imuFail?-1:ESP_OK;
}
inline int plus_i2c_read(''')
        common = common.replace(' assert(addr==0x6b', ''' if(addr==0x19 || addr==0x18){assert(n==1);if(addr==0x18)return -1;
  *out=reg==0x0f?0x11:imuRegs[reg];return ESP_OK;}
 assert(addr==0x6b''')
        common = common.replace('assert(lcdParked && lcdLatched && !locked', 'assert(!storageOff && !sdHeld && !keysMasked && (!railsOff || keysRestoreFail) && lcdParked && !locked')
        common = common.replace(' rtcHeld=true; // Actual', ''' assert(storageOff && sdHeld && keysMasked && railsOff);
 rtcHeld=true; // Actual''')
        common += r'''
inline bool MetalioSd_ParkBusChecked(){assert(storageOff && !lcdParked);sdHeld=true;calls.emplace_back("sd_park");return !sdFail;}
inline bool MetalioSd_ResumeBusChecked(){sdHeld=false;calls.emplace_back("sd_release");return true;}
inline void epd_board_metalio_eink4_plus_tps_enter_sleep(){assert(storageOff && sdHeld);calls.emplace_back("tps_sleep");}
inline void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(){assert(lcdParked && sdHeld);keysMasked=true;calls.emplace_back("keys_mask");}
inline bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(){assert(keysMasked);calls.emplace_back("p17_input");return true;}
inline void epd_board_metalio_eink4_plus_sleep_cut_rails(){assert(storageOff && sdHeld && lcdParked && keysMasked);railsOff=true;calls.emplace_back("rails_cut");}
inline void epd_board_metalio_eink4_plus_sleep_restore_rails(){assert(!keysMasked);railsOff=false;calls.emplace_back("rails_restore");}
inline bool epd_board_metalio_eink4_plus_ioexp_restore_keys_checked(){keysMasked=false;calls.emplace_back("keys_restore");return !keysRestoreFail;}
inline bool epd_board_metalio_eink4_plus_sleep_restore_rails_staged(bool keysOk,void(*trace)(unsigned)){
 if(!keysOk)return false;
 epd_board_metalio_eink4_plus_sleep_restore_rails();for(unsigned n=0;n<6;++n)if(trace)trace(n);return true;
}

inline bool epd_board_metalio_eink4_plus_sleep_io_checked(bool parked){return !(parked?parkCheckFail:restoreCheckFail);}
'''
        code = r'''
#include "fake.h"
#include "PRODUCTION"
using plus_board::StandbyWake;
bool suspendSd(void*){assert(!storageOff);calls.emplace_back("sd_stop");if(suspendFail)return false;storageOff=true;return true;}
bool resumeSd(void*){assert(storageOff && !sdHeld);calls.emplace_back("sd_remount");storageOff=false;return !railsOff && !remountFail;}
void clean(){assert(!storageOff && !sdHeld && !keysMasked && (!railsOff || keysRestoreFail) && !lcdParked && !rtcMux && isrEnabled && !locked);}
void reset(){
 clean();calls.clear();fault.clear();sleeps=lcdParks=lcdRestores=usbRevives=readAt=0;now=0;
 sdFail=suspendFail=remountFail=parkCheckFail=restoreCheckFail=imuFail=keysRestoreFail=false;railsOff=false;
 initialP1=0xff;keyAfterWake=false;hold=false;cause=ESP_SLEEP_WAKEUP_TIMER;irqLowAfterWake=false;irqLow=false;
 plus_board::sleepFault=false;imuRegs[0x20]=0x57;imuRegs[0x22]=0x40;imuRegs[0x24]=8;imuRegs[0x25]=2;imuRegs[0x30]=0x11;
}
int main(){
 assert(plus_board::begin());
 plus_board::StandbyStorage lease{suspendSd,resumeSd,nullptr};
 for(int n=0;n<100;++n){
  reset();assert(plus_board::standbyStaticSleep(60,lease)==StandbyWake::Timer);clean();
  assert(sleeps==1 && timerUs==60000000 && lcdRestores==1 && usbRevives==1);
  std::vector<std::string> wanted{"sd_stop","sd_park","tps_sleep","park_lcd","keys_mask","p17_input","rails_cut","latch_lcd","sleep",
    "keys_restore","rails_restore","sd_release","sd_remount","usb_clock","restore_lcd"};
  size_t next=0;for(auto& name:calls)if(next<wanted.size() && name==wanted[next])++next;assert(next==wanted.size());
  assert(imuRegs[0x20]==0x57 && imuRegs[0x22]==0x40 && imuRegs[0x24]==8 && imuRegs[0x25]==2 && imuRegs[0x30]==0x11);
 }
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;keyAfterWake=true;lcdRestoreMs=1000;
 assert(plus_board::standbyStaticSleep(60,lease)==StandbyWake::PowerButton);clean();lcdRestoreMs=0;
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;
 // OEM latch: an expander IRQ while parked is a key, even when the tap was already
 // released at the first read. One park, one sleep, one restore, then leave Standby.
 assert(plus_board::standbyStaticSleep(60,lease)==StandbyWake::PowerButton);
 assert(sleeps==1 && lcdRestores==1 && usbRevives==1 && std::count(calls.begin(),calls.end(),std::string("rails_restore"))==1);
 assert(std::count(calls.begin(),calls.end(),std::string("rails_cut"))==1);clean();
 reset();cause=ESP_SLEEP_WAKEUP_EXT1;irqLowAfterWake=true;
 assert(plus_board::standbyStaticSleep(60,lease)==StandbyWake::PowerButton && sleeps==1);irqLow=false;clean();
 for(int failure=0;failure<8;++failure){
  reset();switch(failure){case 0:suspendFail=true;break;case 1:sdFail=true;break;case 2:imuFail=true;break;
   case 3:parkCheckFail=true;break;case 4:remountFail=true;break;case 5:restoreCheckFail=true;break;case 6:fault="sleep";break;default:keysRestoreFail=true;}
  // A card that does not remount is HalStorage's to recover (next storage use);
  // the wake itself, and later sleeps, still go ahead.
  const auto wanted=failure==4?StandbyWake::Timer:StandbyWake::Failed;
  assert(plus_board::standbyStaticSleep(60,lease)==wanted);clean();
  assert(sleeps==(failure>=4));
  if(failure==4)assert(plus_board::standbySleepReady() && std::count(calls.begin(),calls.end(),std::string("sd_remount"))==1);
  if(failure==7){
   assert(railsOff && !plus_board::standbySleepReady());
   assert(std::find(calls.begin(),calls.end(),"rails_restore")==calls.end());
   const int before=sleeps;assert(plus_board::standbyStaticSleep(60,lease)==StandbyWake::Failed && sleeps==before);
  }
  if(failure==2 || failure==5)assert(!plus_board::standbySleepReady());
 }
}
'''.replace('PRODUCTION', str(PORT/'firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusBoard.cpp'))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'fake.h').write_text(common)
            for name in ['InputManager.h','PowerManager.h','plus_i2c_wire.h','metalio_i2c1.h',
                         'epd_board_metalio_eink4_plus.h','config.h','esp_log.h','Arduino.h','lcd_fault.h',
                         'driver/gpio.h','driver/rtc_io.h','driver/usb_serial_jtag.h','hal/usb_serial_jtag_ll.h',
                         'esp_sleep.h','metalio_touch.h','HalAudioOutput.h','StandbyDiag.h','metalio_sd.h','esp_private/mspi_timing_tuning.h']:
                f=root/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#include "fake.h"\n')
            (root/'test.cpp').write_text(code)
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-DRICKYOS_PLUS_STANDBY_SLEEP=1','-DRICKYOS_PLUS_STATIC_STANDBY=1','-I'+tmp,
                str(root/'test.cpp'),'-o',str(root/'test')],check=True)
            subprocess.run([str(root/'test')],check=True)

    def test_static_activity_never_ticks_or_redraws_on_timer(self):
        source=(UP/'src/activities/apps/standby/StandbyActivity.cpp').read_text()
        run_cpp(r'''
#include <cassert>
#include <algorithm>
#include <cstdint>
#define FREEINK_DEVICE_METALIO_PLUS 1
#define RICKYOS_PLUS_STATIC_STANDBY 1
#define CROSSPOINT_EMULATED 0
#define STANDBY_DIAG(...) ((void)0)
uint32_t millis(){return 40000;}
struct {bool standbySwipeUnlock=false;unsigned long powerOffMs=6UL*3600UL*1000UL;unsigned long getStandbyPowerOffMs(){return powerOffMs;}} SETTINGS;
namespace bleinput{bool isRunning(){return false;}}
namespace HalRecorder{bool active(){return false;}}
struct {bool isOn(){return false;}} Frontlight;
constexpr int WIFI_MODE_NULL=0;
struct {int getMode(){return 0;}} WiFi;
struct {bool isUsbConnected(){return false;}} gpio;
namespace standby_time{constexpr uint32_t kLightSleepIdleMs=35000;bool shouldLightSleep(uint32_t,bool,bool,bool,bool){return true;}}
struct RenderLock{~RenderLock(){}};
struct HalPowerManager {
 enum class LightSleepWakeReason {Timer,PowerButton,PowerButtonHeld,Failed};
 LightSleepWakeReason reason=LightSleepWakeReason::Timer;int sleeps=0;
 bool canStandbyLightSleep(decltype(gpio)&){return true;}
 // Static faces sleep until auto power-off (6 h, entered at 0, now 40 s): one park per standby.
 uint32_t expect=21561;
 LightSleepWakeReason lightSleepFor(uint32_t seconds){assert(seconds==expect);++sleeps;return reason;}
} powerManager;
struct {int closes=0;void closeStandby(){++closes;}} activityManager;
[[maybe_unused]] constexpr uint32_t kMaxLightSleepSeconds=60;
struct Face {bool clock=false;bool needsPicture(){return false;}bool wantsClock(){return clock;}uint32_t secondsUntilNextWake(){return 3600;}} face;
struct StandbyActivity {
 enum class DisplayMode{Immersive};enum class SyncState{Idle};
 DisplayMode mode_=DisplayMode::Immersive;SyncState syncState_=SyncState::Idle;
 Face* currentFace_=&face;bool drewFrame_=true;uint32_t lastInputMs_=0;unsigned long enteredMs_=0;
 struct {void waitRefreshComplete(){}} renderer;
 int ticks=0;
 bool tryLightSleep(uint32_t);void requestUpdate(){assert(false);}
 void processFaceTick(bool wait){assert(wait && currentFace_->wantsClock());++ticks;}
};
''' + method(source,'bool StandbyActivity::tryLightSleep(') + r'''
int main(){
 StandbyActivity activity;
 // No face parks before its first paint.
 for(bool clock:{false,true}){face.clock=clock;activity.drewFrame_=false;assert(!activity.tryLightSleep(35000) && !powerManager.sleeps);}
 face.clock=false;activity.drewFrame_=true;
 for(int i=0;i<100;++i)assert(activity.tryLightSleep(35000));assert(powerManager.sleeps==100 && !activity.ticks);
 powerManager.reason=HalPowerManager::LightSleepWakeReason::PowerButton;assert(activity.tryLightSleep(35000) && activityManager.closes==1);
 powerManager.reason=HalPowerManager::LightSleepWakeReason::PowerButtonHeld;assert(activity.tryLightSleep(35000));
 // Auto power-off disabled: one day per park; already past the deadline: one second.
 SETTINGS.powerOffMs=0;powerManager.expect=86400;assert(activity.tryLightSleep(35000));
 SETTINGS.powerOffMs=30000;powerManager.expect=1;assert(activity.tryLightSleep(35000));
 assert(!activity.ticks);
 // A clock/date face sleeps one second past its next change, then redraws once.
 face.clock=true;powerManager.reason=HalPowerManager::LightSleepWakeReason::Timer;
 SETTINGS.powerOffMs=6UL*3600UL*1000UL;powerManager.expect=3601;
 assert(activity.tryLightSleep(35000) && activity.ticks==1);
 // ...but never past auto power-off.
 SETTINGS.powerOffMs=30000;powerManager.expect=1;assert(activity.tryLightSleep(35000) && activity.ticks==2);
}
''')

    def test_storage_readonly_handles_positions_and_failure_unwind(self):
        source = (UP / 'lib/hal/HalStorage.cpp').read_text()
        impl = source[source.index('class HalFile::Impl {'):source.index('HalFile::HalFile() = default;')]
        constructors = source[source.index('HalFile::HalFile() = default;'):source.index('HalFile HalStorage::open(')]
        methods = ''.join(method(source, m) for m in [
            'HalStorage::HalStorage()', 'HalStorage::StorageLock::StorageLock()',
            'HalStorage::StorageLock::~StorageLock()', 'HalFile HalStorage::open(',
            'bool HalFile::close()', 'bool HalFile::rename(', 'bool HalFile::isOpen() const',
            'HalFile::operator bool() const', 'size_t HalFile::write(const uint8_t*', 'size_t HalFile::write(uint8_t'])
        fake = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <utility>
using oflag_t=int;
constexpr int O_RDONLY=0,O_RDWR=2,O_CREAT=64,O_TRUNC=128;
using SemaphoreHandle_t=std::recursive_mutex*;
constexpr int portMAX_DELAY=0;
inline thread_local int depth=0;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){static std::recursive_mutex m;return &m;}
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t m,int){m->lock();++depth;}
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m){--depth;m->unlock();}
class String: public std::string {};
class Print {public:virtual ~Print()=default;virtual size_t write(const uint8_t*,size_t){return 0;}virtual size_t write(uint8_t){return 0;}};
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
inline uint32_t fakeNow=1;
inline uint32_t millis(){return fakeNow;}
inline void delay(uint32_t ms){fakeNow+=ms;}
template<class T,class... A>std::unique_ptr<T> makeUniqueNoThrow(A&&...a){return std::make_unique<T>(std::forward<A>(a)...);}
inline std::map<std::string,std::string> files{{"/font","abcdefgh"},{"/book","0123456789"},{"/dir",""},{"/scratch","abcdefgh"}};
inline bool mounted=true,failMount=false,failOpen=false,failSync=false,failClose=false;
inline int mounts=0,stops=0;
class FsFile {
 public:
 std::string name;bool opened=false;uint64_t pos=0;
 FsFile()=default;
 explicit FsFile(const char* path){assert(depth>0 && mounted);name=path;opened=!failOpen && files.count(name);}
 FsFile(FsFile&&)=default;FsFile& operator=(FsFile&&)=default;
 bool isOpen()const{return opened;}
 operator bool()const{return opened;}
 bool isDirectory()const{return name=="/dir";}
 uint64_t position()const{return pos;}
 uint64_t fileSize()const{return files.at(name).size();}
 bool seekSet(uint64_t offset){assert(depth>0);pos=offset;return opened && offset<=fileSize();}
 bool close(){assert(depth>0);opened=false;if(failClose){failClose=false;return false;}return true;}
 size_t write(const uint8_t*,size_t){return 0;}size_t write(uint8_t){return 0;}
 bool rename(const char* path){assert(depth>0);files[path]=files[name];files.erase(name);name=path;return true;}
};
struct Device{bool syncDevice(){assert(depth>0);return !failSync;}};
struct SDManager {
 Device device;
 bool ready(){return mounted;}
 bool exists(const char* path){assert(depth>0);return files.count(path);}
 FsFile open(const char* path,oflag_t){return FsFile(path);}
 Device* rawBlockDevice(){return &device;}
 void shutdown(){assert(depth>0 && mounted);mounted=false;++stops;}
 bool begin(){assert(depth>0);++mounts;mounted=!failMount;return mounted;}
} card;
#define SDCard card
'''
        macros = source[source.index('#define HAL_FILE_WRAPPED_CALL'):source.index('void HalFile::flush()')]
        prefix = '\n#include "fake.h"\n#include "HalStorage.h"\nHalStorage HalStorage::instance;\n' + impl + constructors + macros + methods
        # Use an outer thread lifetime so the blocked-reader test has no dangling references.
        code = prefix + r'''
int main(){
 auto font=Storage.open("/font");auto book=Storage.open("/book");HalFile moved=std::move(book);
 std::atomic<bool> started=false,acquired=false;std::thread reader;
 {HalStorage::StandbySession lease;assert(lease.suspend());assert(!mounted && stops==1);
  reader=std::thread([&]{started=true;HalStorage::StorageLock lock;acquired=true;});
  while(!started)std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!acquired);
  assert(lease.resume() && mounted && mounts==1 && font && moved);assert(!acquired);
 }reader.join();assert(acquired);
 for(const auto& path:{"/font","/book"}){
  // Production private fields stay private; validate positions through a real seek below.
  assert(files.count(path));
 }
 for(int unsupported=0;unsupported<4;++unsupported){
  auto blocker=Storage.open(unsupported==1?"/dir":"/scratch",unsupported==0?O_RDWR:O_RDONLY);
  if(unsupported==2)assert(blocker.rename("/renamed"));
  if(unsupported==3)files.erase("/scratch");
  const int previous=stops;
  {HalStorage::StandbySession lease;assert(!lease.suspend() && mounted && stops==previous && moved);}
  blocker.close();
  if(unsupported==2){files["/scratch"]=files["/renamed"];files.erase("/renamed");}
  if(unsupported==3)files["/scratch"]="abcdefgh";
 }
 failSync=true;{HalStorage::StandbySession lease;assert(!lease.suspend() && mounted && font && moved);}failSync=false;
 failClose=true;{HalStorage::StandbySession lease;assert(!lease.suspend() && mounted && font && moved);}
 // Aborted board setup still remounts through RAII.
 {HalStorage::StandbySession lease;assert(lease.suspend());}assert(mounted && font && moved);
 // A changed/failed file closes cleanly and does not poison the other handle.
 {HalStorage::StandbySession lease;assert(lease.suspend());files["/font"]="x";assert(!lease.resume());assert(!font && moved);}
 font.close();
 {HalStorage::StandbySession lease;assert(lease.suspend());failOpen=true;assert(!lease.resume());failOpen=false;assert(!moved);}
 moved.close();auto last=Storage.open("/book");
 assert(last.seek64(4));
 // A card that does not come back: three mounts, the handle stays parked...
 {HalStorage::StandbySession lease;assert(lease.suspend());const int before=mounts;failMount=true;
  assert(!lease.resume() && mounts==before+3);failMount=false;assert(!last && !mounted);}
 // ...the next storage use within 2 s does not retry, the one after mounts it and reopens the handle in place.
 {HalStorage::StorageLock lock;assert(!mounted);}
 fakeNow+=2000;{HalStorage::StorageLock lock;assert(mounted && last && last.position()==4);}
 // That card is no longer parked in Standby for the rest of the boot.
 {const int previous=stops;HalStorage::StandbySession lease;assert(!lease.suspend() && mounted && stops==previous && last);}
 last.close();assert(depth==0);
}
'''
        # Include the real position/seek wrappers as well and test nonzero offsets.
        code = code.replace('int main(){', method(source, 'bool HalFile::seek64(') + method(source, 'size_t HalFile::position() const') + 'int main(){', 1)
        code = code.replace('std::atomic<bool> started=false', 'assert(font.seek64(3) && moved.seek64(7));\n std::atomic<bool> started=false', 1)
        code = code.replace('assert(!acquired);\n }reader.join()', 'assert(!acquired);assert(font.position()==3 && moved.position()==7);\n }reader.join()')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'fake.h').write_text(fake)
            for name in ['Print.h','common/FsApiConstants.h','freertos/semphr.h']:
                f=root/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#include "fake.h"\n')
            (root/'test.cpp').write_text(code)
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-DFREEINK_DEVICE_METALIO_PLUS=1','-DRICKYOS_PLUS_STATIC_STANDBY=1',
                '-I'+tmp,'-I'+str(UP/'lib/hal'),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
            subprocess.run([str(root/'test')],check=True)

if __name__=='__main__':unittest.main()
