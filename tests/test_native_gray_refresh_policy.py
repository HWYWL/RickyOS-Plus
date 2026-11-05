"""Execute allocation-free regional policy and actual patched epdiy transactions."""
import importlib.util
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream/scripts/tests'))
from test_reading_ui_regressions import ROOT, run_cpp


class NativeGrayRefreshPolicyTest(unittest.TestCase):
    def test_damage_ledgers_and_alignment(self):
        run_cpp(r'''
#include <cassert>
#include <array>
#include "NativeGrayRefreshPolicy.h"
using namespace ricky::gray;
int main() {
  constexpr int w=128,h=96;
  std::array<uint8_t,w*h/2> front{},back{};
  auto write=[&](int x,int y,uint8_t g) {
    auto& byte=front[y*w/2+x/2];
    byte=(byte & (x%2 ? 0x0F : 0xF0)) | (g << (x%2 ? 4 : 0));
  };
  // Aligned bar strip protects both portrait and landscape footer edges.
  auto nav=navigationZone({111,0,17,h},w,h);
  assert(nav.x==96 && nav.width==32);
  assert(navigationZone({0,0,17,h},w,h).width==32);
  assert(navigationZone({0,80,w,16},w,h).y==80);
  assert(navigationZone({0,0,w,16},w,h).height==16);
  assert(navigationZone({111,4,17,h-8},w,h).height==h); // Bezel insets do not disable footer protection.
  assert(navigationZone({4,80,w-8,12},w,h).height==16);
  assert(navigationZone({40,40,16,16},w,h).empty());
  write(3,4,9); write(4,4,6); write(112,90,7);
  auto d=difference(front.data(),back.data(),w,h,nav);
  assert(d.content.x==3 && d.content.width==2 && d.content.height==1);
  assert(d.navigation.x==112 && d.navigation.y==90 && d.navigation.width==1);
  assert(d.contentPixels==2 && d.contentTravel==15 && d.contentCapacity==96*96);
  assert(d.navigationPixels==1 && d.navigationTravel==7 && d.navigationCapacity==32*96);
  Ledger ledger;
  auto plan=ledger.advance(d,nav,8,32);
  assert(plan.count==1 && !plan.steps[0].cleanup);
  assert(ledger.contentFrames==0 && ledger.smallFrames==1 && ledger.navigationFrames==1);
  auto snapshot=ledger;
  assert(ledger.advance({},nav,8,32).count==0);
  assert(ledger.smallFrames==snapshot.smallFrames);
  // Many small changes must not spend the eight content-page cleanups.
  for(int i=1;i<31;++i) {
    plan=ledger.advance({{1,2,3,4},{}},nav,8,32);
    assert(plan.count==1 && !plan.steps[0].cleanup);
  }
  plan=ledger.advance({{5,6,1,1},{}},nav,8,32);
  assert(plan.count==1 && plan.steps[0].cleanup && ledger.smallFrames==0);
  assert(plan.steps[0].area.contains(1,2) && plan.steps[0].area.contains(5,6));
  // Content cleanup protects unchanged navigation and retains its own debt.
  for(int i=0;i<8;++i) plan=ledger.advance({{0,10,96,60},{}},nav,8,32);
  assert(plan.count==1 && plan.steps[0].cleanup);
  assert(plan.steps[0].area.x+plan.steps[0].area.width<=nav.x);
  assert(ledger.navigationFrames==1);
  for(int i=0;i<30;++i) ledger.advance({{}, {110,90,2,2}},nav,8,32);
  plan=ledger.advance({{}, {115,92,1,1}},nav,8,32);
  assert(plan.count==1 && plan.steps[0].cleanup && plan.steps[0].area.x>=nav.x);
  // Concurrent footer change + content cleanup are separate, at most two scans.
  for(int i=0;i<7;++i) ledger.advance({{0,10,96,60},{}},nav,8,32);
  plan=ledger.advance({{0,10,96,60},{110,90,1,1}},nav,8,32);
  assert(plan.count==2 && plan.steps[0].cleanup && !plan.steps[1].cleanup);
  // Failed transactions use a candidate ledger, never mutate the live ledger.
  auto candidate=ledger;
  candidate.advance({{1,1,1,1},{}},nav,8,32);
  assert(ledger.smallFrames==0 && candidate.smallFrames==1);
  // Orientation/layout changes invalidate old regional debts.
  ledger.advance({},navigationZone({0,80,w,16},w,h),8,32);
  assert(ledger.navigationFrames==0 && ledger.contentDebt.empty());
  // Identical, odd nibble and edge pixels are classified against 4-bit, not BW.
  back=front;
  assert(difference(front.data(),back.data(),w,h,nav).content.empty());
  write(127,95,15);
  d=difference(front.data(),back.data(),w,h,nav);
  assert(d.content.empty() && d.navigation.x==127 && d.navigation.width==1);
}
''', include_dirs=(ROOT / 'lib/hal',))

    def test_adaptive_pressure_opt_in_and_legacy_reentry(self):
        run_cpp(r'''
#include <cassert>
#include <array>
#include "NativeGrayRefreshPolicy.h"
using namespace ricky::gray;
int main() {
  constexpr int w=128,h=96;
  const Area nav{96,0,32,h};
  // Sparse text spanning the whole content is not a solid black rectangle.
  std::array<uint8_t,w*h/2> front{},back{};
  front[0]=15; front[95*w/2+47]=0xF0;
  auto d=difference(front.data(),back.data(),w,h,nav);
  assert(d.contentPixels==2 && d.contentTravel==30 && d.content.width==96 && d.content.height==96);
  Ledger ledger;
  Plan plan;
  for(int i=0;i<63;++i) {
    plan=ledger.advance(d,nav,8,32,true);
    assert(plan.count==1 && !plan.steps[0].cleanup);
  }
  // Still bounded: sparse repeated updates cannot skip cleaning forever.
  plan=ledger.advance(d,nav,8,32,true);
  assert(plan.steps[0].cleanup && ledger.contentTravel==0 && ledger.contentFrames==0);
  // Gray travel, not call count: six full black<->white equivalents.
  d.contentTravel=uint64_t{d.contentCapacity}*15;
  for(int i=0;i<5;++i) assert(!ledger.advance(d,nav,8,32,true).steps[0].cleanup);
  assert(ledger.advance(d,nav,8,32,true).steps[0].cleanup);
  d.contentTravel=30;
  // Turning on/off never forgives old debt or invents successful scans.
  for(int i=0;i<7;++i) assert(!ledger.advance(d,nav,8,32).steps[0].cleanup);
  assert(!ledger.advance(d,nav,8,32,true).steps[0].cleanup);
  assert(ledger.contentFrames==8);
  const auto debt=ledger.contentDebt;
  auto candidate=ledger;
  assert(candidate.advance(d,nav,8,32).steps[0].cleanup);
  assert(ledger.contentFrames==8 && !ledger.contentDebt.empty());
  plan=ledger.advance(d,nav,8,32);
  assert(plan.steps[0].cleanup && plan.steps[0].area.contains(debt.x,debt.y));
  // Off is exactly the old 8/32 policy, irrespective of changed-pixel stats.
  for(int i=0;i<7;++i) assert(!ledger.advance(d,nav,8,32).steps[0].cleanup);
  assert(ledger.advance(d,nav,8,32).steps[0].cleanup);
  // Navigation has independent pressure and bounded feedback counts.
  d={}; d.navigation={110,5,1,1}; d.navigationTravel=1; d.navigationCapacity=32*h;
  for(int i=0;i<127;++i) assert(!ledger.advance(d,nav,8,32,true).steps[0].cleanup);
  assert(ledger.advance({},nav,8,32,true).count==0);
  plan=ledger.advance(d,nav,8,32,true);
  assert(plan.steps[0].cleanup && plan.steps[0].area.x>=nav.x && ledger.navigationTravel==0);
  d.navigationTravel=uint64_t{d.navigationCapacity}*15;
  for(int i=0;i<11;++i) assert(!ledger.advance(d,nav,8,32,true).steps[0].cleanup);
  assert(ledger.advance(d,nav,8,32,true).steps[0].cleanup);
  // Both regions independently clean only their debt, at most two steps.
  ledger={}; d.content={0,0,96,96}; d.contentCapacity=96*h;
  d.contentTravel=uint64_t{d.contentCapacity}*15*6;
  d.navigationTravel=uint64_t{d.navigationCapacity}*15*12;
  plan=ledger.advance(d,nav,8,32,true);
  assert(plan.count==2 && plan.steps[0].cleanup && plan.steps[1].cleanup);
  assert(plan.steps[0].area.x+plan.steps[0].area.width<=nav.x);
  assert(ledger.contentTravel==0 && ledger.navigationTravel==0);
}
''', include_dirs=(ROOT / 'lib/hal',))


if __name__ == '__main__':
    unittest.main()
