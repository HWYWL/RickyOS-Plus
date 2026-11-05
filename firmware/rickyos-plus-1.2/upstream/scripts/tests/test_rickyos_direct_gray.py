"""Exercise the real direct-gray transaction and render-task policy, not optics."""
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp
from test_rickyos_gray_settle import HARNESS, SOURCE


class RickyDirectGrayTest(unittest.TestCase):
    def test_direct_transaction_and_failure_fallback(self):
        harness = HARNESS.replace('#include <cassert>', '#include <cassert>\n#include <atomic>')
        harness = harness.replace('bool requested = false, switching = false;',
                                  'std::atomic<bool> requested{false}; bool switching = false;')
        harness = harness.replace('requested', 'requestedUpdate')
        harness = harness.replace('uint32_t generation = 7;',
                                  'uint32_t generation = 7; std::atomic<uint32_t> renderRequestGeneration{0};')
        harness = harness.replace('struct RenderLock {};',
                                  'struct Rect { int x=0,y=0,width=0,height=0; };\nstruct RenderLock {};')
        harness = harness.replace('struct Page {',
                                  'struct Page {\n bool usesMainTabBar() const {return true;}\n'
                                  'struct Layout { Rect tabBar{0,120,32,8}; };\n'
                                  'Layout mainTabLayout() const {return {}; }\n')
        harness = harness.replace('uint32_t& generation;',
                                  'uint32_t& generation; std::atomic<uint32_t>* requests=nullptr; bool newer=false;')
        harness = harness.replace('if (interrupt) ++generation;',
                                  'if (interrupt) ++generation; if(newer) ++*requests;')
        harness = harness.replace('int begins = 0, commits = 0, cancels = 0;',
                                  'int begins = 0, commits = 0, cancels = 0; bool cleanup = false; int uiProfiles=0;\n'
                                  'void setNextGray16UiRefresh(int,int,int,int,uint8_t,uint8_t) {++uiProfiles;}\n'
                                  'bool nextRefreshNeedsCleanup() const { return cleanup; }\n'
                                  'void clearNextRefreshOverride() { cleanup = false; }')
        harness = harness.replace('void settleToGray(RenderLock&&, uint32_t);',
                                  'bool isSwitchPending() { return switching; }\n'
                                  'bool renderDirectGray(RenderLock&&);')
        cases = r'''
int main() {
  ActivityManager m;
  m.renderer.canBegin=false;
  assert(!m.renderDirectGray(RenderLock{}));
  assert(m.page.renders==0 && m.renderer.commits==0 && !m.renderer.capture);
  m.renderer.canBegin=true;
  m.page.requests=&m.renderRequestGeneration;
  // Touch/release alone must not cancel the only foreground frame.
  m.page.interrupt=true;
  assert(m.renderDirectGray(RenderLock{}));
  assert(m.renderer.commits==1 && m.renderer.cancels==0);
  m.page.interrupt=false;
  m.requestedUpdate=true;
  assert(m.renderDirectGray(RenderLock{}));
  m.requestedUpdate=false; m.switching=true;
  assert(m.renderDirectGray(RenderLock{}));
  m.switching=false;
  assert(m.renderer.commits==1 && m.renderer.cancels==2 && !m.renderer.capture);
  // Main loop can consume requestedUpdate during painting, but the newer
  // generation must still suppress this stale frame before any panel scan.
  m.page.newer=true;
  assert(m.renderDirectGray(RenderLock{}));
  assert(m.renderer.commits==1 && m.renderer.cancels==3);
  m.page.newer=false;
  m.page.abandon=true;
  assert(m.renderDirectGray(RenderLock{}));
  m.page.abandon=false;
  assert(m.renderer.commits==1 && !m.renderer.active);
  for(int i=2;i<=16;++i) {
    assert(m.renderDirectGray(RenderLock{}));
    assert(m.renderer.commits==i && !m.renderer.active && !m.renderer.capture);
#ifndef SIMULATOR
    assert(m.renderer.uiProfiles==i);
    assert(m.display.profiles==0); // Driver owns regional cadence, not a whole-screen counter.
#else
    assert(m.display.profiles==0);
#endif
  }
  m.renderer.cleanup=true;
  assert(m.renderDirectGray(RenderLock{}));
  assert(!m.renderer.cleanup);
#ifndef SIMULATOR
  assert(m.display.profile==2);
#endif
  m.renderer.succeeds=false;
  m.renderer.cleanup=true;
  assert(!m.renderDirectGray(RenderLock{}));
  assert(!m.renderer.capture && !m.renderer.active && m.renderer.cleanup);
}
'''
        production = method(SOURCE.read_text(), 'bool ActivityManager::renderDirectGray(')
        for defines in [('FREEINK_DEVICE_READPICO=1',), ('SIMULATOR', 'SIMULATOR_DEVICE_READPICO')]:
            with self.subTest(defines=defines):
                run_cpp(harness + production + cases, include_dirs=(ROOT / 'src',), defines=defines)

    def test_foreground_policy_has_one_submission_and_preserves_reader_idle(self):
        production = method(SOURCE.read_text(), 'void ActivityManager::renderTaskLoop(')
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>
using TaskHandle_t=void*;
constexpr unsigned portMAX_DELAY=9999;
constexpr unsigned kGraySettleDelayMs=ricky::ds::refresh::kGraySettleDelayMs;
constexpr int eIncrement=0;
#define pdTRUE true
#define pdMS_TO_TICKS(ms) (ms)
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
#define LOG_DBG(...) ((void)0)
struct Stop {};
std::vector<unsigned> waits;
std::vector<int> wakes;
int cursor=0, notifications=0;
unsigned ulTaskNotifyTake(bool,unsigned delay) {
  waits.push_back(delay);
  if(cursor==static_cast<int>(wakes.size())) throw Stop{};
  return wakes[cursor++];
}
void xTaskNotify(TaskHandle_t,int,int) {++notifications;}
struct RenderLock {};
struct HalPowerManager {struct Lock {~Lock() {}};};
struct Settings {int screenInverted=0;} SETTINGS;
struct Display {void setInverted(bool) {}};
bool graySettleAllowed() {return !ricky::ds::refresh::kDirectGrayUi;}
struct Page {
  unsigned delay=0;
  bool opted=true;
  int renders=0,idles=0;
  unsigned idleRenderDelayMs() const {return delay;}
  bool wantsGraySettle() const {return opted;}
  void render(RenderLock&&) {++renders;}
  void renderIdle(uint32_t) {++idles;}
};
struct ActivityManager {
  enum class PendingAction {None,Push};
  std::atomic<PendingAction> pendingAction{PendingAction::None};
  std::atomic<uint32_t> idleRenderGeneration{0};
  std::atomic<const char*> lastIdleCancel{"none"};
  std::atomic<bool> lastIdleCancelRearms{false},requestedUpdate{false};
  Display display;
  Page page; Page* currentActivity=&page;
  TaskHandle_t waitingTaskHandle=nullptr;
  bool directSucceeds=true;
  int directs=0,settles=0;
  bool isSwitchPending() const {return pendingAction!=PendingAction::None;}
  bool idleRenderCancelled(uint32_t value) const {
    return value!=idleRenderGeneration || requestedUpdate || isSwitchPending();
  }
  bool renderDirectGray(RenderLock&&) {++directs; return directSucceeds;}
  void settleToGray(RenderLock&&,uint32_t) {++settles;}
  void renderTaskLoop();
};
'''
        cases = r'''
void run(ActivityManager& m,std::vector<int> events) {
  wakes=events; waits.clear(); cursor=0; notifications=0;
  try {m.renderTaskLoop();} catch(Stop&) {}
}
int main() {
  {ActivityManager m; m.waitingTaskHandle=&m; run(m,{1});
#if RICKYOS_UI_DIRECT_GRAY
   assert(m.directs==1 && m.page.renders==0 && waits.back()==portMAX_DELAY);
#else
   assert(m.directs==0 && m.page.renders==1 && waits.back()==1500);
#endif
   assert(m.settles==0 && notifications==1);}
  {ActivityManager m; m.directSucceeds=false; run(m,{1});
   assert(m.page.renders==1);
#if RICKYOS_UI_DIRECT_GRAY
   assert(m.directs==1 && waits.back()==portMAX_DELAY);
#endif
  }
  {ActivityManager m; m.page.delay=400; run(m,{1,0});
   assert(m.directs==0 && m.page.renders==1 && m.page.idles==1 && m.settles==0);
   assert((waits==std::vector<unsigned>{portMAX_DELAY,400,portMAX_DELAY}));}
  {ActivityManager m; m.page.opted=false; run(m,{1});
   assert(m.directs==0 && m.page.renders==1 && waits.back()==portMAX_DELAY);}
  {ActivityManager m; m.pendingAction=ActivityManager::PendingAction::Push; run(m,{1});
   assert(m.directs==0 && m.page.renders==0);}
#if !RICKYOS_UI_DIRECT_GRAY
  {ActivityManager m; run(m,{1,0}); assert(m.settles==1 && m.directs==0);}
#endif
}
'''
        for direct in (0, 1):
            with self.subTest(direct=direct):
                run_cpp(harness + production + cases, include_dirs=(ROOT / 'src',),
                        defines=('FREEINK_DEVICE_READPICO=1', f'RICKYOS_UI_DIRECT_GRAY={direct}'))

    def test_manual_cleanup_preserves_reader_ownership_and_gray_ui(self):
        production = method(SOURCE.read_text(), 'bool ActivityManager::handleForcedRefresh(')
        run_cpp(r'''
#include <cassert>
struct Page {
  bool owns=false,opted=true; unsigned delay=0;
  bool handleForcedRefresh() {return owns;}
  bool wantsGraySettle() const {return opted;}
  unsigned idleRenderDelayMs() const {return delay;}
};
struct Renderer {int full=0; void requestNextFullRefresh() {++full;}};
struct ActivityManager {
  Page page; Page* currentActivity=&page; Renderer renderer;
  int cancels=0,updates=0;
  void cancelIdleRender() {++cancels;}
  void requestUpdate() {++updates;}
  bool handleForcedRefresh();
};
''' + production + r'''
int main() {
  ActivityManager m;
  assert(m.handleForcedRefresh() && m.updates==1 && m.renderer.full==1);
  m.page.owns=true; assert(m.handleForcedRefresh() && m.updates==1);
  m.page.owns=false; m.page.delay=400; assert(!m.handleForcedRefresh());
  m.page.delay=0; m.page.opted=false; assert(!m.handleForcedRefresh());
  m.currentActivity=nullptr; assert(!m.handleForcedRefresh());
  assert(m.cancels==5 && m.updates==1);
}
''', defines=('FREEINK_DEVICE_READPICO=1',))

    def test_cleanup_hint_is_not_consumed_by_query(self):
        production = method((ROOT / 'lib/GfxRenderer/GfxRenderer.h').read_text(),
                            'bool nextRefreshNeedsCleanup(')
        run_cpp(r'''
#include <cassert>
struct HalDisplay {enum RefreshMode {FAST_REFRESH,HALF_REFRESH,FULL_REFRESH};};
struct Renderer {
  bool nextRefreshOverridePending=false;
  HalDisplay::RefreshMode nextRefreshOverride=HalDisplay::FAST_REFRESH;
''' + production + r'''
};
int main() {
  Renderer r; assert(!r.nextRefreshNeedsCleanup());
  r.nextRefreshOverridePending=true; assert(!r.nextRefreshNeedsCleanup());
  r.nextRefreshOverride=HalDisplay::HALF_REFRESH;
  assert(r.nextRefreshNeedsCleanup() && r.nextRefreshOverridePending);
  r.nextRefreshOverride=HalDisplay::FULL_REFRESH; assert(r.nextRefreshNeedsCleanup());
  r.nextRefreshOverridePending=false; assert(!r.nextRefreshNeedsCleanup());
}
''')


if __name__ == '__main__':
    unittest.main()
