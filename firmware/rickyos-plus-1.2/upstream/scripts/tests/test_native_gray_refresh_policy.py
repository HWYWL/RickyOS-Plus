"""Execute allocation-free regional policy and actual patched epdiy transactions."""
import importlib.util
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

from test_reading_ui_regressions import ROOT, run_cpp


class NativeGrayRefreshPolicyTest(unittest.TestCase):
    def test_fresh_sdk_patch_and_repeated_build(self):
        spec = importlib.util.spec_from_file_location('ricky_epd_patch', ROOT / 'scripts/patch_rickyos_epdiy.py')
        patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patch)
        with tempfile.TemporaryDirectory(prefix='ricky-epd-patch-') as directory:
            project = Path(directory)
            for relative in ('libs/display/EpdiyLcd/src/EpdiyLcd.cpp',
                             'libs/display/EpdiyLcd/include/EpdiyLcd.h',
                             'libs/display/EpdiyLcd/src/epdiy/src/output_common/render_context.c'):
                source = subprocess.run(['git', '-C', str(ROOT / 'freeink-sdk'), 'show', 'HEAD:' + relative],
                                        check=True, capture_output=True, text=True).stdout
                destination = project / 'freeink-sdk' / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(source)
            (project / 'lib/hal').mkdir(parents=True)
            shutil.copyfile(ROOT / 'lib/hal/NativeGrayRefreshPolicy.h', project / 'lib/hal/NativeGrayRefreshPolicy.h')
            patch.apply(project)
            paths = [p for p in (project / 'freeink-sdk').rglob('*') if p.is_file()]
            before = {p: p.read_bytes() for p in paths}
            patch.apply(project)
            self.assertEqual(before, {p: p.read_bytes() for p in paths})
            driver = (project / 'freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp').read_text()
            self.assertEqual(driver.count('bool epdiyLcdRestart() {'), 1)
            self.assertEqual(driver.count('bool epdiyLcdCommitGrayscale16('), 1)
            self.assertIn(patch.UI_CONFIG, driver)
            with self.assertRaises(RuntimeError):
                patch.patch_ui(driver.replace('const bool regionalUi = g_gray16Profile == 4;', ''))

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

    def test_patch_is_idempotent_and_driver_transactions(self):
        spec = importlib.util.spec_from_file_location('ricky_epd_patch', ROOT / 'scripts/patch_rickyos_epdiy.py')
        patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patch)
        source = (ROOT / 'freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp').read_text()
        self.assertIn(patch.UI_MARK, source)
        self.assertEqual(patch.patch_ui(source), source)
        header = (ROOT / 'freeink-sdk/libs/display/EpdiyLcd/include/EpdiyLcd.h').read_text()
        self.assertEqual(patch.patch_header(header), header)
        self.assertEqual(header.count('void epdiyLcdSetGray16UiRefresh('), 1)
        # Reuse the real driver/highlevel/OOM harness without editing SDK tests.
        host = ROOT / 'freeink-sdk/libs/display/EpdiyLcd/test/host'
        spec = importlib.util.spec_from_file_location('epd_transactions', host / 'test_transactions.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        if platform.system() == 'Darwin':
            # Apple ld has no GNU --wrap. The driver/highlevel tests themselves
            # are portable; keep GNU allocator/queue fault injection on Linux.
            portable = (host / 'test_transactions.py').read_text()
            portable = portable.replace('str(highlevel), str(queue), str(queue_check),',
                                        'str(highlevel), str(queue),')
            portable = portable.replace('"-Wl,--wrap=calloc", ', '')
            portable = portable[:portable.index('        from test_resources')] + \
                '        print("Driver/highlevel/PMU transactions passed; GNU resource/queue fault checks are Linux-only")\n'
            exec(compile(portable, str(host / 'test_transactions.py'), 'exec'), runner.__dict__)
        with tempfile.TemporaryDirectory(prefix='ricky-ui-transactions-') as directory:
            target = Path(directory)
            shutil.copytree(host, target, dirs_exist_ok=True)
            test = (host / 'transactions.cpp').read_text()
            test = test.replace('int draws = 0, clears = 0;',
                                'int draws = 0, clears = 0, powerOns = 0; bool lastDriven[32]{};')
            test = test.replace('  powered = railsOk;', '  ++powerOns; powered = railsOk;')
            test = test.replace('int, const bool*,\n                                const uint8_t*,',
                                'int, const bool* lines,\n                                const uint8_t* columns,')
            self.assertIn('const bool* lines', test)
            test = test.replace('  lastMode = mode & 0x3F;',
                                '  for(int p=0;p<32;++p) lastDriven[p]=lines[p/16] && '
                                '((columns[(p%16)/2] >> (4*(p&1))) & 15);\n  lastMode = mode & 0x3F;')
            if platform.system() == 'Darwin':
                test = test.replace('  check_queue();', '')
            extra = r'''
extern "C" void epd_lq_stale_take(uint32_t* phases,uint32_t* lines) { *phases=0; *lines=0; }
void checkUiNative(freeink::EpdiyLcdDriver& driver) {
  const int allocations=allocation_calls;
  uint8_t proxy[4]{};
  // Establish white, successful 4-bit baseline.
  auto* frame=driver.beginGrayscale16(); memset(frame,0xFF,16);
  freeink::epdiyLcdSetGray16Profile(2);
  assert(driver.commitGrayscale16(proxy));
  int before=draws;
  const int powerBefore=powerOns;
  frame=driver.beginGrayscale16(); memset(frame,0xFF,16);
  freeink::epdiyLcdSetGray16UiRefresh(0,0,0,0,8,32);
  assert(driver.commitGrayscale16(proxy) && draws==before && powerOns==powerBefore);
  for(int i=1;i<=32;++i) {
    frame=driver.beginGrayscale16(); memset(frame,0xFF,16);
    frame[0]=(i&1) ? 0xF7 : 0xFF;  // Genuine mid-gray feedback, not DU/BW.
    freeink::epdiyLcdSetGray16UiRefresh(0,0,0,0,8,32);
    assert(driver.commitGrayscale16(proxy));
    assert(lastMode==(i==32 ? MODE_GC16 : MODE_GL16));
    for(int p=16;p<32;++p) assert(!lastDriven[p]); // Control cleanup must not drive the unchanged second row.
  }
  before=draws;
  frame=driver.beginGrayscale16(); frame[0]=0;
  freeink::epdiyLcdSetGray16UiRefresh(0,0,0,0,8,32);
  driver.cancelGrayscale16();
  assert(draws==before);
  frame=driver.beginGrayscale16(); memset(frame,0x55,16);
  assert(driver.commitGrayscale16(proxy) && lastMode==MODE_GL16); // cancelled UI configuration not reused
  // Restore the last successful physical gray frame and its proxy, not staging.
  uint8_t retainedProxy[4]{0xFF,0xFF,0xFF,0xFF};
  assert(!freeink::epdiyLcdRestoreGray16Baseline(retainedProxy)); // requires a transaction
  frame=driver.beginGrayscale16(); memset(frame,0xFF,16);
  assert(freeink::epdiyLcdRestoreGray16Baseline(retainedProxy));
  for(int i=0;i<16;++i) assert(frame[i]==0x55);
  assert(memcmp(retainedProxy,proxy,4)==0);
  frame[0]=0; driver.cancelGrayscale16();
  frame=driver.beginGrayscale16();
  assert(freeink::epdiyLcdRestoreGray16Baseline(retainedProxy) && frame[0]==0x55);
  driver.cancelGrayscale16();
  frame=driver.beginGrayscale16(); memset(frame,0xFF,16);
  freeink::epdiyLcdSetGray16UiRefresh(0,0,0,0,8,32);
  failDraw=true;
  assert(!driver.commitGrayscale16(proxy));
  failDraw=false;
  frame=driver.beginGrayscale16();
  assert(!freeink::epdiyLcdRestoreGray16Baseline(retainedProxy)); // unknown physical state
  driver.cancelGrayscale16();
  const int oldClears=clears;
  frame=driver.beginGrayscale16(); memset(frame,0x33,16);
  freeink::epdiyLcdSetGray16UiRefresh(0,0,0,0,8,32);
  assert(driver.commitGrayscale16(proxy) && clears==oldClears+1 && lastMode==MODE_GC16);
  assert(allocation_calls==allocations);
}
'''
            test = test.replace('int main(int argc, char** argv) {', extra + '\nint main(int argc, char** argv) {')
            test = test.replace('  checkNativeGray(driver);', '  checkNativeGray(driver);\n  checkUiNative(driver);')
            (target / 'transactions.cpp').write_text(test)
            runner.HERE = target
            # Companion test modules still resolve the original checked-in harness.
            import sys
            sys.path.insert(0, str(host))
            try:
                runner.run()
            finally:
                sys.path.remove(str(host))


if __name__ == '__main__':
    unittest.main()
