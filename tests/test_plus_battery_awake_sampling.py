"""No-paint charging regression: execute production loop hook, HAL and estimator."""
from pathlib import Path
import unittest

from test_battery_stability import ROOT, SDK, method, run_cpp

PORT = Path(__file__).resolve().parents[1]


class AwakeSamplingTest(unittest.TestCase):
    def test_no_paint_qualification_gaps_errors_contention_and_wrap(self):
        source = (ROOT / 'lib/hal/HalPowerManager.cpp').read_text()
        main = (ROOT / 'src/main.cpp').read_text()
        hook_start = main.index('#if FREEINK_DEVICE_METALIO_PLUS\n  // Charge qualification')
        hook = main[hook_start:main.index('#endif', hook_start) + len('#endif')]
        self.assertLess(main.index('void loop()'), hook_start)
        self.assertLess(hook_start, main.index('  if (activityManager.requiresExclusiveStorageLoop())'))
        run_cpp(r'''
#include "BatteryMonitor.h"
#include "BatteryDisplayFilter.h"
#include "PlusBatteryEstimator.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#define FREEINK_DEVICE_METALIO_PLUS 1
#define FREEINK_DEVICE_READPICO 0
template<class... T> void ignoreLog(T...) {}
#define LOG_INF(...) ignoreLog(__VA_ARGS__)
uint32_t now=0;
uint32_t millis() { return now; }
using namespace plus_board;
unsigned reads=0;
BatteryEstimator estimator;
BatteryEstimate estimate;
BatteryTelemetry telemetry;
BatteryMonitor::BatteryMonitor():_adcPin(-1),_dividerMultiplier(2),_chargeStatusPin(-1){}
BatteryMonitor::Status BatteryMonitor::readStatus() const {
  ++reads;
  estimate=estimator.update(telemetry, now);
  Status s;
  s.percentage=estimate.percent;
  s.percentageKnown=estimate.known;
  s.percentageSource=estimate.source==BatteryEstimateSource::UncalibratedVoltage
      ? Status::PercentageSource::VoltageEstimate : Status::PercentageSource::CapacityEstimate;
  s.millivolts=telemetry.mv;
  s.chargingKnown=telemetry.currentKnown;
  s.charging=telemetry.current>5;
  return s;
}
bool BatteryMonitor::readPercentageChecked(uint16_t&) const { return false; }
uint16_t BatteryMonitor::readPercentage() const { return 0; }
namespace BoardConfig {
struct Gauge { unsigned gaugeAddr=1; };
struct Board { Gauge batteryGauge; } ACTIVE;
}
struct HalPowerManager {
  mutable BatteryDisplayFilter batteryDisplay;
  mutable std::atomic_flag batteryPollBusy=ATOMIC_FLAG_INIT;
  mutable std::atomic<uint16_t> batteryDisplayPercent{101};
  mutable uint32_t batteryDiagnosticAt=0;
  mutable bool batteryDiagnosticLogged=false;
  mutable int _batteryCachedPercent=0;
  mutable unsigned long _batteryLastPollMs=0;
  uint32_t batteryAwakePollAt=0;
  bool batteryAwakePolled=false;
  static constexpr unsigned long BATTERY_POLL_MS=1500;
  void pollBatteryWhileAwake();
  uint16_t getBatteryPercentage() const;
};
''' + method(source, 'void HalPowerManager::pollBatteryWhileAwake()') +
            method(source, 'uint16_t HalPowerManager::getBatteryPercentage() const') +
            '\nvoid loopTick(HalPowerManager& powerManager) {\n' + hook + r'''
}
void reset() {
  now=0; reads=0; estimator=BatteryEstimator{}; estimate={};
  telemetry={4300,12,12,100,true,true,true,true};
}
bool anchored() { return estimator.checkpoint().version==1; }
int main() {
  // The loop is the only caller: no page renders or explicit UI reads.
  reset(); HalPowerManager idle;
  for(now=0;now<180000;++now) loopTick(idle);
  assert(reads==6 && !anchored());
  loopTick(idle);
  assert(reads==7 && anchored() && estimate.source==BatteryEstimateSource::CoulombCount);
  assert(estimate.percent==100);
  assert(idle.getBatteryPercentage()<=100 && reads==7); // UI shares the existing cache.
  for(unsigned i=0;i<100;++i) loopTick(idle);
  assert(reads==7); // Repeated loop ticks are not extra samples.

  reset(); HalPowerManager discharge;
  telemetry.current=telemetry.averageCurrent=-30;
  for(now=0;now<=180000;now+=1000) loopTick(discharge);
  assert(!anchored()); // High voltage alone still cannot qualify full.

  reset(); HalPowerManager error;
  for(now=0;now<90000;now+=30000) loopTick(error);
  telemetry={}; now=90000; loopTick(error);
  telemetry={4300,12,12,100,true,true,true,true};
  for(now=120000;now<300000;now+=30000) { loopTick(error); assert(!anchored()); }
  now=300000; loopTick(error); assert(anchored()); // Need a new uninterrupted 180 s window.

  reset(); HalPowerManager gap;
  for(now=0;now<=60000;now+=30000) loopTick(gap);
  const unsigned beforeGap=reads;
  now=600000; loopTick(gap);
  assert(reads==beforeGap+1 && !anchored()); // No sleep/blocked-time catch-up.
  for(now=630000;now<780000;now+=30000) { loopTick(gap); assert(!anchored()); }
  now=780000; loopTick(gap); assert(anchored());

  reset(); HalPowerManager busy;
  loopTick(busy); busy.batteryPollBusy.test_and_set();
  now=30000; loopTick(busy); assert(reads==1);
  busy.batteryPollBusy.clear(); now=60000; loopTick(busy); assert(reads==2);
  for(now=90000;now<=180000;now+=30000) loopTick(busy);
  assert(anchored()); // Shared sampler contention does not create a second reader.

  reset(); HalPowerManager wrap;
  const uint32_t start=UINT32_MAX-60000;
  for(uint32_t dt=0;dt<180000;dt+=1000) { now=start+dt; loopTick(wrap); }
  assert(reads==6 && !anchored());
  now=start+180000; loopTick(wrap);
  assert(reads==7 && anchored());
  puts("PASS Plus awake-only sampling: no-paint full anchor, cadence, shared cache, discharge rejection, errors, gaps, contention and wrap");
}
''', SDK / 'include', ROOT / 'lib/hal',
                PORT / 'firmware/rickyos-plus-1.2/components/rickyos_platform/include')


if __name__ == '__main__':
    unittest.main()
