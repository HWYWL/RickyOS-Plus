"""Manufacturer reference and production coulomb estimator; no hardware writes."""
from pathlib import Path
import argparse
import bisect
import hashlib
import json
import unittest

from test_battery_stability import run_cpp

PORT = Path(__file__).resolve().parents[1]
PLATFORM = PORT / 'firmware/rickyos-plus-1.2/components/rickyos_platform'
INCLUDES = PLATFORM / 'include'
WORKBOOK_SHA256 = '45d5698e685162faa8ff470ed0f434163b2c09d7be7af812b7f7b48396ffd291'


class BatteryCurveTest(unittest.TestCase):
    def test_estimator_rebound_charge_sleep_errors_reset_wrap_and_restore(self):
        run_cpp(r'''
#include "PlusBatteryEstimator.h"
#include <cassert>
#include <cstdio>
using namespace plus_board;
BatteryTelemetry telemetry(uint16_t mv=3702,int16_t current=-1000,uint16_t count=100) {
 return {mv,current,current,count,true,true,true,true};
}
BatteryCheckpoint saved(uint16_t count=100,uint32_t remain=9354) {
 BatteryCheckpoint s{1,remain,count,0,0};s.checksum=batteryCheckpointChecksum(s);return s;
}
int main() {
 static_assert(sizeof(BatteryEstimator)<=80,"No heap/large state");
 static_assert(sizeof(BatteryCheckpoint)==16,"Fixed checkpoint format");
 assert(BATTERY_NOMINAL_MAH==2000 && BATTERY_REFERENCE_DECI_MAH==18707);
 assert(batteryLoadedPermille(3300)==0 && batteryLoadedPermille(3702)==500);
 assert(batteryLoadedPermille(4211)==1000);
 unsigned previous=0;
 for(unsigned mv=2500;mv<=5000;++mv) {
  auto p=batteryLoadedPermille(mv);assert(p>=previous && p<=1000);previous=p;
 }
 BatteryEstimator e;
 assert(e.update(telemetry(3702,-50),0).source==BatteryEstimateSource::UncalibratedVoltage);
 for(unsigned t=5000;t<35000;t+=5000)
  assert(e.update(telemetry(3702,-1000,100+t/3600),t).source==BatteryEstimateSource::UncalibratedVoltage);
 auto result=e.update(telemetry(3702,-1000,110),35000);
 assert(result.known && result.percent==50 && result.source==BatteryEstimateSource::LoadedCurve);
 // Voltage sag/rebound with no charge removed cannot create/cancel capacity.
 for(unsigned t=40000;t<100000;t+=5000) {
  auto s=telemetry(t%10000?3500:4100,-50,110);
  result=e.update(s,t);assert(result.percent==50 && result.source==BatteryEstimateSource::CoulombCount);
 }
 result=e.update(telemetry(3800,-50,297),3600000);assert(result.percent==40);
 // Reverse counter delta during actual charge: 187 mAh added, not current * long gap.
 result=e.update(telemetry(4200,100,110),7200000);assert(result.percent==50);
 BatteryEstimator reboot;assert(reboot.restore(e.checkpoint()));
 assert(reboot.update(telemetry(3900,-50,297),0).percent==40);
 auto invalid=telemetry();invalid.counterKnown=false;
 assert(!reboot.update(invalid,5000).known);
 assert(reboot.update(telemetry(3900,-50,298),10000).percent==40);
 // Gauge FC clear is not a 288 mAh top-up.
 result=reboot.update(telemetry(4200,20,0),15000);
 assert(result.source==BatteryEstimateSource::UncalibratedVoltage && reboot.checkpoint().version==0);
 BatteryEstimator full;
 for(unsigned t=0;t<180000;t+=5000)
  assert(full.update(telemetry(4300,30,0),t).source==BatteryEstimateSource::UncalibratedVoltage);
 result=full.update(telemetry(4300,30,0),180000);
 assert(result.percent==100 && result.source==BatteryEstimateSource::CoulombCount);
 // A flash/intermittent voltage read is not sufficient full-charge evidence.
 BatteryEstimator interrupted;
 for(unsigned t=0;t<180000;t+=5000) interrupted.update(telemetry(4300,30,0),t);
 interrupted.update({},180000);
 assert(interrupted.update(telemetry(4300,30,0),185000).source==BatteryEstimateSource::UncalibratedVoltage);
 BatteryEstimator sparse;
 sparse.update(telemetry(4300,30,0),0);
 assert(sparse.update(telemetry(4300,30,0),180000).source==BatteryEstimateSource::UncalibratedVoltage);
 // A current reading failure cannot establish a reference.
 BatteryEstimator noCurrent;auto s=telemetry(4300,30,0);s.currentKnown=false;
 for(unsigned t=0;t<300000;t+=5000)
  assert(noCurrent.update(s,t).source==BatteryEstimateSource::UncalibratedVoltage);
 BatteryEstimator cold;auto profile=telemetry();profile.gaugeReady=false;
 for(unsigned t=0;t<300000;t+=5000)
  assert(cold.update(profile,t).source==BatteryEstimateSource::UncalibratedVoltage);
 BatteryEstimator low;assert(low.restore(saved()));
 for(unsigned t=0;t<=30000;t+=5000) result=low.update(telemetry(3290,-30),t);
 assert(result.percent==0);
 BatteryEstimator impossible;assert(impossible.restore(saved()));
 impossible.update(telemetry(),0);
 assert(impossible.update(telemetry(3800,-50,500),5000).source==BatteryEstimateSource::UncalibratedVoltage);
 // Unsigned RCC and millisecond wrap, without mistaking FC reset for wrap.
 BatteryEstimator wrap;assert(wrap.restore(saved(65535)));
 wrap.update(telemetry(3800,-50,65535),UINT32_MAX-1000);
 assert(wrap.update(telemetry(3800,-50,0),3999).source==BatteryEstimateSource::CoulombCount);
 assert(wrap.checkpoint().remainingDeciMah==9344);
 BatteryEstimator chargeWrap;assert(chargeWrap.restore(saved(0)));
 chargeWrap.update(telemetry(4000,100,0),0);
 assert(chargeWrap.update(telemetry(4000,100,65535),5000).source==BatteryEstimateSource::CoulombCount);
 assert(chargeWrap.checkpoint().remainingDeciMah==9364);
 auto corrupt=saved();corrupt.checksum^=1;assert(!cold.restore(corrupt));
 corrupt=saved();corrupt.counter^=1;assert(!cold.restore(corrupt));
 corrupt=saved();corrupt.remainingDeciMah^=1;assert(!cold.restore(corrupt));
 corrupt=saved();corrupt.version=2;corrupt.checksum=batteryCheckpointChecksum(corrupt);assert(!cold.restore(corrupt));
 corrupt=saved();corrupt.remainingDeciMah=30000;corrupt.checksum=batteryCheckpointChecksum(corrupt);assert(!cold.restore(corrupt));
 // Restored implausible reset must not manufacture a fully charged pack.
 BatteryEstimator reset;assert(reset.restore(saved(1000)));
 assert(reset.update(telemetry(3800,-50,0),0).source==BatteryEstimateSource::UncalibratedVoltage);
 BatteryEstimator frozen;assert(frozen.restore(saved()));
 for(unsigned t=0;t<=20000;t+=5000) result=frozen.update(telemetry(3702,-1000,100),t);
 assert(frozen.checkpoint().version==0 && result.source==BatteryEstimateSource::UncalibratedVoltage);
 puts("PASS physical battery estimate, curve qualification, counter and checkpoint boundaries");
}
''', INCLUDES)

    def test_nvs_owner_write_budget_errors_and_corrupt_checkpoint(self):
        run_cpp(r'''
#include "PlusBatteryEstimator.h"
#include <nvs.h>
#include <cassert>
#include <cstring>
unsigned writes=0;bool fail=false;
int nvs_open(const char*,int,nvs_handle_t* h) {*h=1;return 0;}
int nvs_get_blob(nvs_handle_t,const char*,void* p,size_t*) {std::memset(p,0,16);return 0;}
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t) {++writes;return fail?-1:0;}
int nvs_commit(nvs_handle_t) {return 0;}
void nvs_close(nvs_handle_t) {}
using namespace plus_board;
int main() {
 BatteryTelemetry s{4300,20,20,100,true,true,true,true};
 for(unsigned t=0;t<=180000;t+=5000) estimateBattery(s,t);
 assert(writes==1);
 for(unsigned i=0;i<1000;++i) estimateBattery(s,180000);
 assert(writes==1);
 s={3900,-50,-50,200,true,true,true,true};estimateBattery(s,540000);assert(writes==2);
 s.counter=201;estimateBattery(s,545000);assert(writes==2);
 estimateBattery(s,1140000);assert(writes==3);
 fail=true;s.counter=301;estimateBattery(s,1500000);assert(writes==4);
 for(unsigned t=1505000;t<1530000;t+=5000) estimateBattery(s,t);
 assert(writes==4);estimateBattery(s,1530000);assert(writes==5);
 fail=false;estimateBattery(s,1560000);assert(writes==6);
 s.counterKnown=false;estimateBattery(s,2160000);assert(writes==6);
}
''', INCLUDES, extra_sources=[PLATFORM / 'PlusBattery.cpp'], headers={
            'nvs.h': '''#pragma once
#include <cstddef>
using nvs_handle_t=unsigned;using esp_err_t=int;
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1;
inline const char* esp_err_to_name(int) {return "test";}
int nvs_open(const char*,int,nvs_handle_t*);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_commit(nvs_handle_t);void nvs_close(nvs_handle_t);
''',
            'esp_log.h': '''#pragma once
template<class... T> void ignoreLog(T...) {}
#define ESP_LOGI(...) ignoreLog(__VA_ARGS__)
#define ESP_LOGW(...) ignoreLog(__VA_ARGS__)
''',
        })


def replay_workbook(path):
    """Optional local evidence; never upload the private workbook or raw records."""
    import openpyxl
    if hashlib.sha256(path.read_bytes()).hexdigest() != WORKBOOK_SHA256:
        raise SystemExit('Workbook SHA256 mismatch; review protocol before deriving a new model')
    workbook = openpyxl.load_workbook(path, read_only=True, data_only=True)
    records = list(workbook['Record'].iter_rows(min_row=1500, max_row=2174, values_only=True))
    assert len(records) == 675 and all(row[0:2] == (2, 6) for row in records)
    total = records[-1][7]
    assert total == 1870.7 and all(-1001 < row[5] < -999 for row in records)
    capacities = [row[7] for row in records]
    knots = []
    for percent in range(101):
        target = total * (100-percent) / 100
        index = bisect.bisect_left(capacities, target)
        mv = records[0][4] if index == 0 else (
            records[index-1][4] + (records[index][4]-records[index-1][4]) *
            (target-capacities[index-1]) / (capacities[index]-capacities[index-1]))
        knots.append('{%d,%d}' % (round(mv), percent*10))
    samples = '\n'.join('{%d,%d,%d},' % (round(row[4]), round(row[7]), round((1-row[7]/total)*1000))
                        for row in records)
    code = r'''
#include "PlusBatteryEstimator.h"
#include <cassert>
#include <cmath>
#include <cstdio>
struct Record {unsigned mv,count,expectedPermille;};
Record records[]={SAMPLES};
plus_board::BatteryCurvePoint regenerated[]={KNOTS};
int main() {
 using namespace plus_board;
 static_assert(sizeof(regenerated)==sizeof(BATTERY_DISCHARGE_CURVE));
 for(unsigned i=0;i<101;++i) {
  assert(regenerated[i].mv==BATTERY_DISCHARGE_CURVE[i].mv);
  assert(regenerated[i].permille==BATTERY_DISCHARGE_CURVE[i].permille);
 }
 BatteryEstimator e;
 double maxCurveError=0,maxCountError=0,linearError=0;
 for(unsigned i=0;i<sizeof(records)/sizeof(records[0]);++i) {
  auto r=records[i];
  double curve=std::abs(int(batteryLoadedPermille(r.mv))-int(r.expectedPermille))/10.0;
  if(curve>maxCurveError) maxCurveError=curve;
  uint16_t old=0;batteryPercentFromMv(r.mv,old);
  double linear=std::abs(int(old)*10-int(r.expectedPermille))/10.0;
  if(linear>linearError) linearError=linear;
  auto result=e.update({uint16_t(r.mv),-1000,-1000,uint16_t(r.count),true,true,true,true},i*10000u);
  if(i>=3) {
   assert(result.source!=BatteryEstimateSource::UncalibratedVoltage);
   double count=std::abs(int(result.percent)*10-int(r.expectedPermille))/10.0;
   if(count>maxCountError) maxCountError=count;
  }
 }
 printf("Workbook replay: 675 samples, max curve residual %.2f pp, counter residual %.2f pp, legacy residual %.2f pp\n",
        maxCurveError,maxCountError,linearError);
 // This is reconstruction of the input trace, NOT independent validation or field accuracy.
 assert(maxCurveError<0.5 && maxCountError<1.0);
}
'''.replace('SAMPLES', samples).replace('KNOTS', ','.join(knots))
    run_cpp(code, INCLUDES)
    print(json.dumps({'source_sha256': WORKBOOK_SHA256, 'rows': 'Record!A1500:L2174',
                      'samples': len(records), 'tested_discharge_mah': total,
                      'nominal_mah': 2000, 'validation': 'in-sample reconstruction, not field accuracy'}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--workbook', type=Path)
    args, unittest_args = parser.parse_known_args()
    if args.workbook:
        replay_workbook(args.workbook)
    else:
        unittest.main(argv=[__file__, *unittest_args])
