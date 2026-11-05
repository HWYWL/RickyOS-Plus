"""Compile production battery policy and PMU paths with bounded fake telemetry."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
if not (ROOT / 'lib/hal/BatteryDisplayFilter.h').exists():
    ROOT = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'
PLUS = 'metalio-e-ink4-plus' in str(ROOT)
SDK = ROOT / 'freeink-sdk/libs/hardware/BatteryMonitor'


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_cpp(code, *includes, extra_sources=(), headers=None):
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / 'test.cpp'
        source.write_text(code)
        binary = Path(directory) / 'test'
        for name, content in (headers or {}).items():
            (Path(directory) / name).write_text(content)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + directory,
                        *['-I' + str(path) for path in includes],
                        str(source), *map(str, extra_sources), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


class BatteryStabilityTest(unittest.TestCase):
    def test_production_display_filter(self):
        run_cpp(r"""
#include "BatteryDisplayFilter.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using F=BatteryDisplayFilter;
using S=F::Sample;
S sample(unsigned p, unsigned source=1, int charge=0) {
  return {uint16_t(p),uint8_t(source),true,charge>=0,charge==1};
}
void hold(F& f,unsigned p,unsigned first,unsigned last,unsigned source=1,int charge=0) {
  for(uint32_t t=first;t<=last;t+=5000) f.update(sample(p,source,charge),t);
}
int main() {
  static_assert(sizeof(F)<=64,"Keep battery policy fixed-size and allocation-free");
  F unknown; assert(!unknown.known());
  unknown.update({},0); assert(!unknown.known());
  assert(unknown.update(sample(0),5000)==0 && unknown.known()); // Real zero.
  assert(unknown.update({},10000)==0 && unknown.known()); // Read error retains zero.
  F f; assert(f.update(sample(80),0)==80);
  for(unsigned t=1;t<5000;++t) assert(f.update(sample(20),t)==80); // Paint rate != sample rate.
  assert(f.update(sample(20),5000)==80);
  hold(f,80,10000,60000); assert(f.value()==80); // Isolated spike.
  assert(f.update(sample(0),65000)==80); // One false low.
  assert(f.update(sample(80),70000)==80);
  assert(f.update(sample(0),75000)==80);
  assert(f.update(sample(0),80000)==0); // Genuine repeated low must not count down slowly.
  F normal; normal.update(sample(60),0);
  hold(normal,59,5000,60000); assert(normal.value()==59);
  F jump; jump.update(sample(80),0);
  hold(jump,60,5000,55000); assert(jump.value()==80);
  hold(jump,60,60000,65000); assert(jump.value()==60); // Confirmed correction.
  F reverse; reverse.update(sample(50),0);
  hold(reverse,70,5000,145000); assert(reverse.value()==50);
  hold(reverse,70,150000,155000); assert(reverse.value()==70); // Not locked forever.
  F sources; sources.update(sample(80),0);
  for(unsigned t=5000;t<=150000;t+=5000)
    sources.update(sample(t%10000 ? 30:80,t%10000 ? 2:1),t);
  assert(sources.value()==80); // Alternating SoC/voltage never jumps.
  hold(sources,30,155000,220000,2); assert(sources.value()==30); // Stable fallback eventually accepted.
  F plug; plug.update(sample(90),0);
  hold(plug,99,5000,25000,1,1); assert(plug.value()==90); // Plug-in voltage transient.
  hold(plug,90,30000,60000,1,1); assert(plug.value()==90);
  F sleep; sleep.update(sample(90),0);
  sleep.update(sample(40),3600000); assert(sleep.value()==90);
  hold(sleep,40,3605000,3635000); assert(sleep.value()==90); // No sleep-time credit.
  hold(sleep,40,3640000,3650000); assert(sleep.value()==40);
  F interrupted; interrupted.update(sample(70),0);
  hold(interrupted,40,5000,50000);
  interrupted.update({},55000);
  assert(interrupted.update(sample(40),60000)==70); // Invalid read breaks confirmation.
  F wrap; const uint32_t start=UINT32_MAX-1000;
  wrap.update(sample(77),start);
  assert(!wrap.shouldPoll(start+4999));
  assert(wrap.shouldPoll(start+5000));
  assert(wrap.update(sample(77),start+5000)==77);
  F invalid; invalid.update(sample(101),0); assert(!invalid.known());
  puts("PASS battery policy: zero, unknown, errors, spikes, sampling, source/power changes, low, sleep and wrap");
}
""", ROOT / 'lib/hal')

    @unittest.skipIf(PLUS, 'Read Pico only')
    def test_pmu_frame_preserves_zero_unknown_and_rejects_bad_voltage_crc(self):
        source = (ROOT / 'freeink-sdk/libs/hardware/BoardReadPico/src/BoardReadPico.cpp').read_text()
        run_cpp(r"""
#include <cassert>
#include <cstdint>
#include <cstring>
bool g_pmuPresent=true,transport=true,checksum=true;
constexpr unsigned kPmuQuickBatterySize=8,kPmuRegQuickBattery=0;
constexpr uint16_t kPmuSocUnknown=0xffff;
uint8_t bytes[8]={0xa0,0x0f,0,0,1,3,0,0};
bool pmuReadRegister(unsigned,uint8_t* p,unsigned n) {std::memcpy(p,bytes,n);return transport;}
bool crcOk(const uint8_t*,unsigned,uint16_t) {return checksum;}
uint16_t rd16(const uint8_t* p) {return p[0]|uint16_t(p[1])<<8;}
""" + method(source, 'bool pmuBattery(') + r"""
int main() {
  uint16_t mv=0,soc=123;uint8_t charge=0;
  assert(pmuBattery(mv,soc,charge)&&mv==4000&&soc==0); // Empty != unknown.
  bytes[5]=1;assert(pmuBattery(mv,soc,charge)&&soc==0xffff);
  bytes[5]=3;bytes[2]=255;bytes[3]=255;assert(pmuBattery(mv,soc,charge)&&soc==0xffff);
  bytes[2]=0xe9;bytes[3]=3;assert(pmuBattery(mv,soc,charge)&&soc==0xffff);
  bytes[2]=0xe8;assert(pmuBattery(mv,soc,charge)&&soc==1000);
  checksum=false;assert(!pmuBattery(mv,soc,charge));
  checksum=true;bytes[0]=bytes[1]=0;assert(!pmuBattery(mv,soc,charge));
  bytes[0]=0xa0;bytes[1]=0x0f;bytes[5]=0;assert(!pmuBattery(mv,soc,charge));
  bytes[5]=3;transport=false;assert(!pmuBattery(mv,soc,charge));
  transport=true;g_pmuPresent=false;assert(!pmuBattery(mv,soc,charge));
}
""")

    @unittest.skipIf(PLUS, 'Read Pico only')
    def test_voltage_fallback_monotonic_continuous_and_bounded(self):
        run_cpp(r"""
#include "PmuBatterySample.h"
#include <cassert>
int main() {
  using namespace freeink::battery;
  assert(!validPackMillivolts(0)&&!validPackMillivolts(65535));
  unsigned previous=0;
  for(unsigned mv=2500;mv<=5000;++mv) {
    unsigned p=readPicoVoltagePercent(mv);
    assert(p>=previous&&p<=100&&p-previous<=1);previous=p;
  }
  assert(readPicoVoltagePercent(3820)==50);
  assert(readPicoVoltagePercent(3845)==55);
}
""", SDK / 'include')

    def test_actual_hal_polling_and_unknown_start(self):
        source = (ROOT / 'lib/hal/HalPowerManager.cpp').read_text()
        run_cpp(r"""
#include "BatteryMonitor.h"
#include "BatteryDisplayFilter.h"
#include <atomic>
#include <cassert>
#define FREEINK_DEVICE_READPICO 1
template<class... T> void ignoreLog(T...) {}
#define LOG_INF(...) ignoreLog(__VA_ARGS__)
uint32_t now=0;uint32_t millis() {return now;}
unsigned reads=0;BatteryMonitor::Status fake;
BatteryMonitor::BatteryMonitor():_adcPin(-1),_dividerMultiplier(2),_chargeStatusPin(-1){}
BatteryMonitor::Status BatteryMonitor::readStatus() const {++reads;return fake;}
bool BatteryMonitor::readPercentageChecked(uint16_t&) const {return false;}
uint16_t BatteryMonitor::readPercentage() const {return 0;}
namespace BoardConfig {
struct Gauge {unsigned gaugeAddr=1;};
struct Board {Gauge batteryGauge;};Board ACTIVE;
}
struct HalPowerManager {
 mutable BatteryDisplayFilter batteryDisplay;
 mutable std::atomic_flag batteryPollBusy=ATOMIC_FLAG_INIT;
 mutable std::atomic<uint16_t> batteryDisplayPercent{101};
 mutable uint32_t batteryDiagnosticAt=0;mutable bool batteryDiagnosticLogged=false;
 mutable int _batteryCachedPercent=0;mutable unsigned long _batteryLastPollMs=0;
 static constexpr unsigned long BATTERY_POLL_MS=1500;
 uint16_t getBatteryPercentage() const;
};
""" + method(source, 'uint16_t HalPowerManager::getBatteryPercentage() const') + r"""
int main() {
 HalPowerManager h;
 assert(h.getBatteryPercentage()==101&&reads==1); // Pending stays internal, not a fabricated 0%.
 now=100;fake.percentageKnown=true;fake.percentage=81;
 fake.percentageSource=BatteryMonitor::Status::PercentageSource::StateOfCharge;
 assert(h.getBatteryPercentage()==101&&reads==1);
 now=5000;assert(h.getBatteryPercentage()==81&&reads==2);
 h.batteryPollBusy.test_and_set();now=10000;
 assert(h.getBatteryPercentage()==81&&reads==2); // Concurrent caller cannot start another I2C read.
 h.batteryPollBusy.clear();fake.percentageKnown=false;
 assert(h.getBatteryPercentage()==81&&reads==3);
 now=11000;assert(h.getBatteryPercentage()==81&&reads==3);
}
""", SDK / 'include', ROOT / 'lib/hal')

    def test_actual_battery_label_handles_unknown_and_real_zero(self):
        source = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text()
        run_cpp(r"""
#include <cassert>
#include <cstdint>
#include <string>
namespace UiHighDpiProfile {constexpr bool enabled=true;constexpr int controlGap=8;}
struct Rect {int x,y,width,height;};
struct GfxRenderer {
 mutable std::string text;
 void drawText(int,int,int,const char* s) const {text=s;}
 int getTextWidth(int,const char* s) const {return std::string(s).size()*10;}
};
// Actual font geometry is exercised by test_plus_battery_alignment.py. This
// fixture isolates telemetry labels (pending, real zero and full) from layout.
namespace ricky::battery {
inline int iconTop(const GfxRenderer&,int,int textY,int) {return textY+8;}
}
struct Power {uint16_t percent=101;uint16_t getBatteryPercentage(){return percent;}} powerManager;
struct BaseTheme {
 static constexpr int STATUS_NUMERIC_FONT_ID=1,batteryPercentSpacing=4;
 void drawBatteryLeft(const GfxRenderer&,Rect,bool) const;
 void drawBatteryRight(const GfxRenderer&,Rect,bool,int) const;
 void drawBatteryOutline(const GfxRenderer&,int,int,int,int) const {}
 void fillBatteryIcon(const GfxRenderer&,Rect,uint16_t) const {}
};
""" + method(source, 'void BaseTheme::drawBatteryLeft(') +
                method(source, 'void BaseTheme::drawBatteryRight(') + r"""
int main() {
 GfxRenderer r;BaseTheme t;
 for(unsigned p: {101u,0u,100u}) {
   powerManager.percent=p;std::string expected=p<=100?std::to_string(p)+"%":"";
   r.text.clear();t.drawBatteryLeft(r,{0,0,32,20},true);assert(r.text==expected);
   r.text.clear();t.drawBatteryRight(r,{200,0,32,20},true,1);assert(r.text==expected);
 }
}
""")

    def test_actual_status_read_uses_one_sample_and_keeps_unknown_fields(self):
        source = (SDK / 'src/BatteryMonitor.cpp').read_text()
        prefix = r"""
#include "BatteryMonitor.h"
#include <cassert>
#include <cstdint>
#define FREEINK_BATTERY_I2C_GAUGE 1
template<class... T> void ignoreLog(T...) {}
#define ESP_LOGI(...) ignoreLog(__VA_ARGS__)
namespace BoardConfig {
enum class GaugeType {Cw32L010Pmu};
struct Gauge {uint8_t gaugeAddr=0x55;GaugeType gaugeType=GaugeType::Cw32L010Pmu;};
struct Board {Gauge batteryGauge;};Board ACTIVE;
}
BatteryMonitor::BatteryMonitor():_adcPin(-1),_dividerMultiplier(2),_chargeStatusPin(-1){}
bool BatteryMonitor::hasM5Pm1Backend() const {return false;}
bool BatteryMonitor::hasAdcBackend() const {return false;}
bool BatteryMonitor::readM5Pm1Status(Status&) const {return false;}
uint16_t BatteryMonitor::readMillivolts() const {return 0;}
uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t) {return 0;}
bool readGaugeSoc(uint16_t&) {return false;}
bool readGaugeMillivolts(uint16_t&) {return false;}
bool readGaugeCharging(bool& known) {known=false;return false;}
int digitalRead(int) {return 0;}
int chargeActiveLevel() {return 0;}
"""
        if not PLUS:
            prefix += r"""
#include "PmuBatterySample.h"
unsigned reads=0;bool ok=true;uint16_t rawSoc=0,rawMv=4000;uint8_t rawCharge=1;
bool readPmuBattery(uint16_t& mv,uint16_t& soc,uint8_t& charge) {
 ++reads;mv=rawMv;soc=rawSoc;charge=rawCharge;return ok;
}
"""
            main = r"""
int main() {
 BatteryMonitor b;
 auto s=b.readStatus();assert(reads==1&&s.percentageKnown&&s.percentage==0);
 assert(s.percentageSource==BatteryMonitor::Status::PercentageSource::StateOfCharge);
 rawSoc=0xffff;rawMv=3845;s=b.readStatus();assert(reads==2&&s.percentage==55);
 assert(s.percentageSource==BatteryMonitor::Status::PercentageSource::VoltageEstimate);
 rawCharge=0;s=b.readStatus();assert(!s.chargingKnown);
 rawCharge=2;s=b.readStatus();assert(s.chargingKnown&&s.charging);
 rawCharge=3;s=b.readStatus();assert(s.charging);
 rawCharge=4;s=b.readStatus();assert(s.chargingKnown&&!s.charging);
 rawMv=0;s=b.readStatus();assert(!s.percentageKnown&&!s.millivoltsKnown);
 rawMv=4000;ok=false;s=b.readStatus();assert(!s.percentageKnown&&!s.chargingKnown);
}
"""
            includes = [SDK / 'include']
        else:
            prefix += r"""
#include "PlusBattery.h"
#include "PlusBatteryEstimator.h"
#define FREEINK_DEVICE_METALIO_PLUS 1
constexpr uint8_t BQ27220_VOLTAGE=8,BQ27220_CURRENT=12,BQ27220_STATE_OF_CHARGE=44;
uint16_t rawMv=3815,rawCurrent=0;bool voltageOk=true,currentOk=true;unsigned readCount=0,diag=0;
uint32_t now=0;uint32_t millis() {return now;}
plus_board::BatteryEstimator estimator;
plus_board::BatteryEstimate plus_board::estimateBattery(const BatteryTelemetry& sample,uint32_t t) {
 return estimator.update(sample,t);
}
bool readReg16(uint8_t,uint8_t reg,uint16_t& out) {
 ++readCount;
 if(reg==8) {out=rawMv;return voltageOk;}
 if(reg==12) {out=rawCurrent;return currentOk;}
 if(reg==0x14) {out=rawCurrent;return currentOk;}
 if(reg==0x22) {out=0;return true;}
 if(reg==0x3a) {out=0x20;return true;}
 ++diag;out=0xffff;return true; // Invalid raw gauge SoC must never enter the display.
}
"""
            main = r"""
int main() {
 BatteryMonitor b;auto s=b.readStatus();
 assert(s.percentageKnown&&s.percentage==50&&s.millivolts==3815&&!s.chargingKnown);
 assert(s.percentageSource==BatteryMonitor::Status::PercentageSource::VoltageEstimate);
 assert(diag==4);unsigned first=readCount;
 now=5000;rawCurrent=100;s=b.readStatus();
 assert(s.chargingKnown&&s.charging&&readCount==first+5&&diag==4);
 now=10000;rawCurrent=uint16_t(int16_t(-50));s=b.readStatus();
 assert(s.chargingKnown&&!s.charging);
 currentOk=false;s=b.readStatus();assert(!s.chargingKnown&&s.percentageKnown);
 uint16_t percent=999;assert(b.readPercentageChecked(percent)&&percent==50);
 assert(b.readPercentage()==50);
 rawMv=0;s=b.readStatus();assert(!s.percentageKnown&&!s.millivoltsKnown);
 percent=999;assert(!b.readPercentageChecked(percent)&&percent==999);
 rawMv=3815;voltageOk=false;s=b.readStatus();assert(!s.percentageKnown);
}
"""
            includes = [SDK / 'include', ROOT.parent / 'components/rickyos_platform/include']
        run_cpp(prefix + method(source, 'BatteryMonitor::Status BatteryMonitor::readStatus() const') +
                method(source, 'uint16_t BatteryMonitor::readPercentage() const') +
                method(source, 'bool BatteryMonitor::readPercentageChecked(') + main,
                *includes)


if __name__ == '__main__':
    unittest.main()
