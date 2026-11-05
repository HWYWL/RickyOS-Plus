"""Exercise the actual HAL frequency/lock code and the S31 clock selector.

No sleep, GPIO, charger, or device access. Clock-source selection in the SDK
is software evidence only; current draw and peripheral timing need hardware.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_battery_stability import method, run_cpp

PORT = Path(__file__).resolve().parents[1]
FW = PORT / 'firmware/rickyos-plus-1.2'
HAL = FW / 'upstream/lib/hal'


class PlusIdlePowerTest(unittest.TestCase):
    def test_actual_probe_is_read_only_decodes_signed_current_and_releases_bus(self):
        source = (FW / 'components/rickyos_application/port/PlusBoard.cpp').read_text()
        body = method(source, 'void traceIdlePower()')
        fixture = r'''
#define RICKYOS_PLUS_IDLE_FREQ_MHZ 80
#define RICKYOS_STANDBY_DIAG 1
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
constexpr int ESP_OK=0;
constexpr bool kStandbySleepEnabled=false;
struct Settings {unsigned rickyStandbyFace=2,rickyPowerOffScreen=2;unsigned long getStandbyPowerOffMs(){return 21600000UL;}} SETTINGS;
namespace BoardConfig {struct Gauge {uint8_t gaugeAddr=0x55;};struct Board {Gauge batteryGauge;};Board ACTIVE;}
bool locked=false,lockFails=false;int reads=0,failRead=0;
std::vector<std::string> notes;
template<class... T> void note(const char* format,T... args){char out[112];int n=snprintf(out,sizeof(out),format,args...);assert(n>=0&&n<int(sizeof(out)));notes.emplace_back(out);}
#define STANDBY_DIAG(...) note(__VA_ARGS__)
unsigned getCpuFrequencyMhz(){return 80;}
bool metalio_i2c1_lock(int ms){assert(ms==20&&!locked);if(lockFails)return false;locked=true;return true;}
void metalio_i2c1_unlock(){assert(locked);locked=false;}
int plus_i2c_read(uint8_t addr,uint8_t reg,uint8_t* out,unsigned len){
  assert(locked&&addr==0x55&&len==2);
  const unsigned regs[]={0x08,0x0c,0x14,0x22};
  const uint16_t values[]={4300,uint16_t(-57),uint16_t(-59),65535};
  assert(reg==regs[reads%4]);const auto word=values[reads%4];++reads;
  out[0]=uint8_t(word);out[1]=uint8_t(word>>8);return reads==failRead?-1:ESP_OK;
}
BODY
int main(){
 traceIdlePower();assert(!locked&&reads==4&&notes.size()==2);
 assert(notes.back()=="idlepower: cpu=80 mv=4300 i=-57 avg=-59 q=65535");
 for(int n=1;n<=4;++n){reads=0;failRead=n;notes.clear();traceIdlePower();assert(!locked&&reads==n);assert(notes.back()=="idlepower: cpu=80 read_error=1");}
 reads=0;failRead=0;lockFails=true;traceIdlePower();assert(!locked&&reads==0);
 lockFails=false;BoardConfig::ACTIVE.batteryGauge.gaugeAddr=0;traceIdlePower();assert(!locked&&reads==0);
}
'''.replace('BODY', body)
        run_cpp(fixture)

    def test_production_idle_frequency_wifi_locks_failures_and_other_targets(self):
        header = (HAL / 'HalPowerManager.h').read_text()
        declaration = header[header.index('class HalPowerManager {'):]
        # Expose fields only in the fixture, preserving the real declarations.
        declaration = declaration.replace('class HalPowerManager {',
                                          'class HalPowerManager { public:', 1)
        source = (HAL / 'HalPowerManager.cpp').read_text()
        bodies = '\n'.join(method(source, signature) for signature in (
            'void HalPowerManager::setPowerSaving(',
            'HalPowerManager::Lock::Lock()',
            'HalPowerManager::Lock::~Lock()',
        ))
        fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
using SemaphoreHandle_t = std::mutex*;
constexpr int portMAX_DELAY = -1, WIFI_MODE_NULL = 0;
struct HalGPIO {};
struct BatteryDisplayFilter {};
struct MockWifi {int mode=0; int getMode() {return mode;}} WiFi;
struct MockBle {bool active=false; bool isRunning() {return active;}} BleHid;
namespace plus_board {
bool audioBusy=false;
bool idleCpuFrequencyAllowed() {return !audioBusy;}
}
std::mutex modeMutex;
std::atomic<unsigned> actualMhz{320}, transitions{0}, activeTransitions{0};
bool rejectNext=false;
inline void xSemaphoreTake(SemaphoreHandle_t m, int) {m->lock();}
inline void xSemaphoreGive(SemaphoreHandle_t m) {m->unlock();}
inline bool setCpuFrequencyMhz(unsigned value) {
  assert(activeTransitions.fetch_add(1)==0); // APB/SPI callbacks never overlap.
  ++transitions;
  std::this_thread::yield();
  const bool ok=!rejectNext;
  rejectNext=false;
  if(ok) actualMhz=value;
  --activeTransitions;
  return ok;
}
inline unsigned getCpuFrequencyMhz() {return actualMhz;}
template<class... T> void ignoreLog(T...) {}
#define LOG_DBG(...) ignoreLog(__VA_ARGS__)
#define LOG_INF(...) ignoreLog(__VA_ARGS__)
#define LOG_ERR(...) ignoreLog(__VA_ARGS__)
class HalPowerManager;
extern HalPowerManager powerManager;
DECLARATION
HalPowerManager powerManager;
BODIES
int main() {
  static_assert(HalPowerManager::LOW_POWER_FREQ == EXPECTED_IDLE);
  auto& p=powerManager;
  p.normalFreq=320; p.modeMutex=&modeMutex;
  p.setPowerSaving(true);
  assert(actualMhz==EXPECTED_IDLE && p.isLowPower && transitions==1);
  p.setPowerSaving(true);
  assert(transitions==1); // Repeated idle polling does not reprogram clocks.
  {
    HalPowerManager::Lock render;
    assert(actualMhz==320 && !p.isLowPower);
    for(int n=0;n<1000;++n) p.setPowerSaving(true);
    assert(actualMhz==320); // Render/SD work keeps its normal-speed owner.
  }
  p.setPowerSaving(true);
  assert(actualMhz==EXPECTED_IDLE && p.isLowPower);
  WiFi.mode=1;p.setPowerSaving(true);
  assert(actualMhz==320 && !p.isLowPower);
  WiFi.mode=0;p.setPowerSaving(true);
  assert(actualMhz==EXPECTED_IDLE);
#if FREEINK_DEVICE_METALIO_PLUS && defined(RICKYOS_PLUS_IDLE_FREQ_MHZ) && RICKYOS_PLUS_IDLE_FREQ_MHZ == 40
  plus_board::audioBusy=true;p.setPowerSaving(true);
  assert(actualMhz==320 && !p.isLowPower);
  plus_board::audioBusy=false;p.setPowerSaving(true);
  assert(actualMhz==EXPECTED_IDLE);
#endif
#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
  BleHid.active=true;p.setPowerSaving(true);
  assert(actualMhz==320 && !p.isLowPower);
  BleHid.active=false;p.setPowerSaving(true);
#endif
  rejectNext=true;p.setPowerSaving(false);
  assert(actualMhz==EXPECTED_IDLE && p.isLowPower); // Failure never changes HAL state.
  p.setPowerSaving(false);assert(actualMhz==320 && !p.isLowPower);
  rejectNext=true;p.setPowerSaving(true);
  assert(actualMhz==320 && !p.isLowPower);
  p.setPowerSaving(true);assert(actualMhz==EXPECTED_IDLE && p.isLowPower);
  // Exercise the real mutex path under competing awake-idle/foreground requests.
  auto requests=[](bool idle) {for(int n=0;n<1000;++n) powerManager.setPowerSaving(idle);};
  std::thread idle(requests,true),foreground(requests,false);
  idle.join();foreground.join();
  p.setPowerSaving(false);assert(actualMhz==320 && !p.isLowPower);
  assert(activeTransitions==0);
  const unsigned before=transitions;
  p.normalFreq=0;p.setPowerSaving(true);assert(transitions==before);
}
'''.replace('DECLARATION', declaration).replace('BODIES', bodies)
        cases = (
            '#define FREEINK_DEVICE_METALIO_PLUS 1\n#define BOARD_HAS_PSRAM 1\n'
            '#define CONFIG_IDF_TARGET_ESP32S31 1\n#define EXPECTED_IDLE 80\n',
            '#define FREEINK_DEVICE_METALIO_PLUS 1\n#define BOARD_HAS_PSRAM 1\n'
            '#define CONFIG_IDF_TARGET_ESP32S31 1\n#define RICKYOS_PLUS_IDLE_FREQ_MHZ 40\n'
            '#define EXPECTED_IDLE 40\n',
            '#define FREEINK_DEVICE_METALIO_PLUS 1\n#define BOARD_HAS_PSRAM 1\n'
            '#define CONFIG_IDF_TARGET_ESP32S31 1\n#define RICKYOS_PLUS_IDLE_FREQ_MHZ 80\n'
            '#define EXPECTED_IDLE 80\n',
            '#define FREEINK_DEVICE_METALIO_PLUS 0\n#define BOARD_HAS_PSRAM 0\n'
            '#define CONFIG_IDF_TARGET_ESP32C3 1\n#define FREEINK_CAP_BLE_HID_HOST 1\n'
            '#define EXPECTED_IDLE 10\n',
            '#define FREEINK_DEVICE_METALIO_PLUS 0\n#define BOARD_HAS_PSRAM 1\n'
            '#define EXPECTED_IDLE 80\n',
        )
        for definitions in cases:
            with self.subTest(definitions=definitions):
                run_cpp(definitions + fixture)

    def test_cmake_rejects_invalid_or_mixed_sleep_comparisons(self):
        cmake = shutil.which('cmake')
        if not cmake:
            cmake = str(Path.home() / '.platformio/packages/tool-cmake/bin/cmake')
        self.assertTrue(Path(cmake).is_file(), 'CMake is required to check build guards')
        source = (FW / 'components/rickyos_application/CMakeLists.txt').read_text()
        # Standby sleep is on by default; the idle comparison must switch it off.
        start = source.index('set(RICKYOS_PLUS_DEFAULT_ON')
        end = source.index('# Experiment, off by default', start)
        guard = source[start:end]
        preamble = 'set(COMPONENT_LIB fixture)\nfunction(target_compile_definitions)\nendfunction()\n'
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'check.cmake'
            script.write_text(preamble + guard)
            for freq, sleep, success in (('', '', True), ('40', '0', True), ('80', '0', True),
                                         ('40', '', False), ('40', '1', False), ('80', '1', False),
                                         ('10', '0', False), ('40;80', '0', False)):
                env = dict(os.environ, RICKYOS_PLUS_IDLE_FREQ_MHZ=freq,
                           RICKYOS_PLUS_STANDBY_SLEEP=sleep, RICKYOS_PLUS_STATIC_STANDBY=sleep)
                result = subprocess.run([cmake, '-P', str(script)], env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, success, result.stderr)

    def test_actual_s31_sdk_selects_xtal_at_40_and_cpll_at_80(self):
        sdk = Path(os.environ.get('RICKYOS_IDF_PATH', PORT / 'tools/esp-idf-v6.1'))
        source = sdk / 'components/esp_hw_support/port/esp32s31/rtc_clk.c'
        if not source.is_file():
            self.skipTest('Set RICKYOS_IDF_PATH to the actual S31 SDK')
        body = method(source.read_text(), 'bool rtc_clk_cpu_freq_mhz_to_config(')
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef enum {SOC_CPU_CLK_SRC_XTAL, SOC_CPU_CLK_SRC_CPLL, SOC_CPU_CLK_SRC_PLL_F240M} soc_cpu_clk_src_t;
typedef struct {uint32_t integer,denominator,numerator;} hal_utils_clk_div_t;
typedef struct {soc_cpu_clk_src_t source;hal_utils_clk_div_t div;uint32_t source_freq_mhz,freq_mhz;} rtc_cpu_freq_config_t;
#define CLK_LL_PLL_320M_FREQ_MHZ 320
#define CLK_LL_PLL_240M_FREQ_MHZ 240
static uint32_t rtc_clk_xtal_freq_get(void) {return 40;}
BODY
int main(void) {
  rtc_cpu_freq_config_t c;
  assert(rtc_clk_cpu_freq_mhz_to_config(40,&c));
  assert(c.source==SOC_CPU_CLK_SRC_XTAL && c.freq_mhz==40 && c.div.integer==1);
  assert(rtc_clk_cpu_freq_mhz_to_config(80,&c));
  assert(c.source==SOC_CPU_CLK_SRC_CPLL && c.freq_mhz==80 && c.div.integer==4);
  assert(rtc_clk_cpu_freq_mhz_to_config(320,&c));
  assert(c.source==SOC_CPU_CLK_SRC_CPLL && c.freq_mhz==320 && c.div.integer==1);
  assert(!rtc_clk_cpu_freq_mhz_to_config(0,&c));
  assert(!rtc_clk_cpu_freq_mhz_to_config(27,&c));
}
'''.replace('BODY', body)
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / 'clock.c'
            exe = Path(directory) / 'clock'
            src.write_text(fixture)
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
