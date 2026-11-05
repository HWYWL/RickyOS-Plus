"""Execute the production idle gray transaction with hardware/simulator seams.

These checks establish scheduling/transaction behavior, not panel ghosting or
waveform latency. The 1.2 device acceptance remains mandatory.
"""

import unittest
from pathlib import Path

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'src/activities/ActivityManager.cpp'

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <utility>
#include "components/RickyDesignTokens.h"
template <typename... Args> void logDebug(Args&&...) {}
#define LOG_DBG(...) logDebug(__VA_ARGS__)
unsigned long millis() { return 0; }
struct RenderLock {};
struct Renderer {
  bool canBegin = true, active = false, capture = false, succeeds = true;
  int begins = 0, commits = 0, cancels = 0;
  bool beginGrayscale16() { ++begins; return active = canBegin; }
  void setDisplayCapture(bool on) { capture = on; }
  bool isGrayscale16Active() { return active; }
  void cancelGrayscale16() { ++cancels; active = false; }
  bool commitGrayscale16() { ++commits; active = false; return succeeds; }
};
struct Display {
  int profile = 0, profiles = 0;
  void setNextGray16Profile(int next) { profile = next; ++profiles; }
};
struct Page {
  Renderer& renderer;
  uint32_t& generation;
  bool interrupt = false, abandon = false;
  int renders = 0;
  void render(RenderLock&&) {
    assert(renderer.capture && renderer.active);
    ++renders;
    if (interrupt) ++generation;
    if (abandon) renderer.cancelGrayscale16();
  }
};
struct ActivityManager {
  Renderer renderer;
  Display display;
  uint32_t generation = 7;
  bool requested = false, switching = false;
  Page page{renderer, generation};
  Page* currentActivity = &page;
  bool idleRenderCancelled(uint32_t value) {
    return generation != value || requested || switching;
  }
  void settleToGray(RenderLock&&, uint32_t);
};
'''

CASES = r'''
int main() {
  ActivityManager manager;
  // An unavailable gray buffer cannot replace the existing B/W frame.
  manager.renderer.canBegin = false;
  manager.settleToGray(RenderLock{}, manager.generation);
  assert(manager.page.renders == 0 && manager.renderer.commits == 0);
  assert(!manager.renderer.capture);

  manager.renderer.canBegin = true;
  // Input arriving during drawing discards the stale transaction.
  manager.page.interrupt = true;
  manager.settleToGray(RenderLock{}, manager.generation);
  assert(manager.renderer.cancels == 1 && manager.renderer.commits == 0);
  assert(!manager.renderer.capture && !manager.renderer.active);
  manager.page.interrupt = false;

  // Queued redraws and Activity switches have the same cancellation barrier.
  manager.requested = true;
  manager.settleToGray(RenderLock{}, manager.generation);
  manager.requested = false;
  manager.switching = true;
  manager.settleToGray(RenderLock{}, manager.generation);
  manager.switching = false;
  assert(manager.renderer.cancels == 3 && manager.renderer.commits == 0);
  assert(manager.display.profiles == 0);

  // A page can cancel its transaction; the manager must not commit it again.
  manager.page.abandon = true;
  manager.settleToGray(RenderLock{}, manager.generation);
  manager.page.abandon = false;
  assert(manager.renderer.commits == 0 && !manager.renderer.capture);

  for (int i = 1; i <= 16; ++i) {
    manager.settleToGray(RenderLock{}, manager.generation);
    assert(manager.renderer.commits == i);
    assert(!manager.renderer.capture && !manager.renderer.active);
#if !defined(SIMULATOR)
    assert(manager.display.profiles == i);
    assert(manager.display.profile ==
           (i % ricky::ds::refresh::kGc16EverySettles == 0 ? 2 : 1));
#else
    assert(manager.display.profiles == 0);
#endif
  }
  manager.renderer.succeeds = false;
  manager.settleToGray(RenderLock{}, manager.generation);
  assert(manager.renderer.commits == 17 && !manager.renderer.capture);
}
'''


class RickyGraySettleTest(unittest.TestCase):
    def test_hardware_transaction_cancellation_and_clean_cadence(self):
        run_cpp(HARNESS + method(SOURCE.read_text(), 'void ActivityManager::settleToGray(') + CASES,
                include_dirs=(ROOT / 'src',), defines=('FREEINK_DEVICE_READPICO=1',))

    def test_simulator_transaction_has_no_hardware_profile_call(self):
        run_cpp(HARNESS + method(SOURCE.read_text(), 'void ActivityManager::settleToGray(') + CASES,
                include_dirs=(ROOT / 'src',), defines=('SIMULATOR', 'SIMULATOR_DEVICE_READPICO'))

    def test_only_opted_in_quiet_pages_are_armed(self):
        source = SOURCE.read_text()
        self.assertIn('bool graySettleAllowed() { return !ricky::ds::refresh::kDirectGrayUi; }', source)
        self.assertIn('kGraySettleDelayMs = ricky::ds::refresh::kGraySettleDelayMs', source)
        self.assertIn('delayMs == 0 && currentActivity->wantsGraySettle() && graySettleAllowed()', source)
        foreground = method(source, 'void ActivityManager::renderTaskLoop(')
        self.assertIn('else if (!idleRenderCancelled(idleGeneration))', foreground)
        self.assertIn('settleToGray(std::move(lock), idleGeneration)', foreground)
        self.assertNotIn('beginGrayscale16()', foreground)
        self.assertIn('#if FREEINK_DEVICE_READPICO || defined(SIMULATOR_DEVICE_READPICO)',
                      (ROOT / 'src/activities/ActivityManager.h').read_text())

    def test_locked_design_tokens(self):
        run_cpp(r'''
#include <cassert>
#include "components/RickyDesignTokens.h"
int main() {
  using namespace ricky::ds;
  assert(refresh::kGraySettleDelayMs == 1500 && refresh::kGc16EverySettles == 8);
  assert(gray::oneBitInk(gray::kInk2) && gray::oneBitInk(gray::kMute));
  assert(gray::oneBitInk(gray::kLine) && !gray::oneBitInk(gray::kTint));
  assert(!type::kAllowSyntheticBold && !refresh::kDitherInChrome);
  assert(component::kAppTile.radius == radius::kControl);
}
''', include_dirs=(ROOT / 'src',))


if __name__ == '__main__':
    unittest.main()
