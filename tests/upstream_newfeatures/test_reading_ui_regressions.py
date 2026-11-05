"""Run production home geometry, headers, resource errors and sync refresh with small seams."""
from pathlib import Path
import importlib.util
import json
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WEREAD = ROOT / 'src/activities/apps/weread/webapi'


def method(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# The sources are RickyOS-only (Read Pico, Chinese, high-density UI): every check builds
# with the same product switches the firmware does, whatever the test asks for besides.
PRODUCT_DEFINES = ('RICKYOS_PRODUCT=1', 'ENABLE_CHINESE_VERSION=1', 'CROSSMUX_UI_PROFILE_HIGH_DPI=1')


def run_cpp(program, include_dirs=(), defines=()):
    defines = tuple(d for d in PRODUCT_DEFINES
                    if d.split('=')[0] not in {x.split('=')[0] for x in defines}) + tuple(defines)
    with tempfile.TemporaryDirectory(prefix='reading-ui-') as directory:
        cpp = Path(directory) / 'check.cpp'
        exe = Path(directory) / 'check'
        cpp.write_text(f'#include "{ROOT}/src/components/UiHighDpiProfile.h"\n'
                       f'#include "{ROOT}/src/components/RickyDesignTokens.h"\n' + program)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        *('-I'+str(path) for path in include_dirs), *('-D'+define for define in defines), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


class ReadingUiRegressionTest(unittest.TestCase):
    def test_high_dpi_controls_share_geometry_and_leave_gaps(self):
        run_cpp(r'''
#include <cassert>
#include <initializer_list>
#include "activities/MainTab.h"
#include "InxItemLayout.h"
int main() {
  for (const Rect safe : {Rect{5,5,674,1203}, Rect{5,8,1203,674},
                         Rect{5,8,674,1203}, Rect{8,5,1203,674},
                         Rect{5,5,590,987}, Rect{5,8,987,590},
                         Rect{5,8,590,987}, Rect{8,5,987,590}}) {
    for (bool bottom : {false,true}) {
      auto layout=MainTabs::layout(safe,0,96,bottom,bottom?48:0);
      assert(layout.tabBar.y>=safe.y && layout.tabBar.y+96<=safe.y+safe.height);
      assert(layout.content.width==safe.width && layout.content.height>0);
      if (bottom) {
        assert(layout.content.y-layout.statusBar.y-layout.statusBar.height>=12);
        assert(layout.tabBar.y-layout.content.y-layout.content.height>=12);
      } else assert(layout.content.y>=layout.tabBar.y+layout.tabBar.height);
      for (int tab=0;tab<5;++tab) {
        auto bounds=MainTabs::tabBounds(tab,safe.width);
        assert(bounds.right-bounds.left>=96);
        assert(MainTabs::fromX((bounds.left+bounds.right)/2,safe.width)==MainTabs::values[tab]);
        assert(MainTabs::fromX(bounds.right,safe.width)==MainTab::None);
        if (tab<4) assert(MainTabs::tabBounds(tab+1,safe.width).left-bounds.right>=6);
      }
    }
    for (int slot=0;slot<12;++slot) {
      auto cell=InxGridGeometry::cellBounds(slot,safe.width,safe.height);
      assert(InxGridGeometry::indexFromPoint(cell.x+cell.width/2,cell.y+cell.height/2,
                                           safe.width,safe.height,0,12)==slot);
      assert(InxGridGeometry::indexFromPoint(cell.x+cell.width,cell.y+cell.height/2,
                                           safe.width,safe.height,0,12)==-1);
      if (slot%3<2) {
        auto next=InxGridGeometry::cellBounds(slot+1,safe.width,safe.height);
        assert(next.x-cell.x-cell.width>=12);
      }
    }
  }
}
''', include_dirs=(ROOT / 'src',), defines=('CROSSMUX_UI_PROFILE_HIGH_DPI',))

    def test_high_dpi_status_text_and_battery_fit_top_tab_footer(self):
        source = (ROOT / 'src/components/themes/inx/InxTheme.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <initializer_list>
struct Rect { int x,y,width,height; };
constexpr int SMALL_FONT_ID=1, STATUS_NUMERIC_FONT_ID=2;
struct CrossPointSettings {
  enum { INX_TAB_TOP, INX_TAB_BOTTOM };
  enum class HIDE_BATTERY_PERCENTAGE { SHOW,HIDE_ALWAYS };
};
struct { int inxTabPosition=0, clockFormat=0; CrossPointSettings::HIDE_BATTERY_PERCENTAGE hideBatteryPercentage{}; } SETTINGS;
struct UITheme {
  struct Metrics { int batteryWidth=32,batteryHeight=20; };
  static UITheme& getInstance() { static UITheme theme; return theme; }
  const Metrics& getMetrics() { static Metrics metrics; return metrics; }
};
namespace TimeUtils { void formatCurrentTime(char*,unsigned long,bool) {} }
struct GfxRenderer {
  int width=684,height=1216; mutable Rect clip{};
  struct ClipScope { ClipScope(const GfxRenderer& r,int x,int y,int w,int h) { r.clip={x,y,w,h}; } };
  int getScreenWidth() const { return width; } int getScreenHeight() const { return height; }
  int getLineHeight(int) const { return 34; }
  void drawText(int,int,int,const char*) const {}
};
struct InxTheme {
  void drawMainTabStatusBar(const GfxRenderer&,Rect) const;
  void drawBatteryRight(const GfxRenderer& r,Rect rect,bool,int font) const {
    assert(font==SMALL_FONT_ID && rect.y>=r.clip.y);
    assert(rect.y+34<=r.clip.y+r.clip.height);
    assert(rect.y+8+rect.height<=r.clip.y+r.clip.height);
    assert(rect.x>=r.clip.x && rect.x+rect.width<=r.clip.x+r.clip.width);
  }
};
''' + method(source, 'void InxTheme::drawMainTabStatusBar(') + r'''
int main() {
  for (auto size : {Rect{0,0,684,1216},Rect{0,0,1216,684},
                    Rect{0,0,600,1000},Rect{0,0,1000,600}}) {
    GfxRenderer renderer; renderer.width=size.width; renderer.height=size.height;
    SETTINGS.inxTabPosition=CrossPointSettings::INX_TAB_TOP;
    InxTheme{}.drawMainTabStatusBar(renderer,{5,size.height-48,size.width-10,40});
    SETTINGS.inxTabPosition=CrossPointSettings::INX_TAB_BOTTOM;
    InxTheme{}.drawMainTabStatusBar(renderer,{5,5,size.width-10,48});
  }
}
''', defines=('CROSSMUX_UI_PROFILE_HIGH_DPI',))

    def test_high_dpi_builtin_font_cache_identity(self):
        settings = (ROOT / 'src/CrossPointSettings.cpp').read_text()
        sizes = (ROOT / 'src/ReaderFontSizes.cpp').read_text()
        program = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <initializer_list>
#include "fontIds.h"
constexpr uint8_t BUILTIN_READER_POINT_SIZES[]={12};
struct CrossPointSettings {
  enum { NOTOSANS=1 };
  char sdFontFamilyName[8]{}; void* sdFontResolverCtx=nullptr;
  int (*sdFontIdResolver)(void*,const char*,uint8_t)=nullptr;
  uint8_t fontPointSize=12; int fontFamily=NOTOSANS;
  int getReaderFontId() const;
};
''' + method(sizes, 'uint8_t snapToNearestPointSize(') + method(settings, 'int CrossPointSettings::getReaderFontId(') + r'''
int main() {
  CrossPointSettings settings;
  for (int family : {0,1}) for (uint8_t size : {12,14,18}) {
    settings.fontFamily=family; settings.fontPointSize=size;
    int id=settings.getReaderFontId();
    if (UiHighDpiProfile::enabled) {
      assert(id==UiHighDpiProfile::reader12FontId);
      assert(id==0x4738000C);
      assert(id!=0x4737000C && id!=0x4737000E);
      assert(id!=NOTOSANS_12_FONT_ID && id!=NOTOSERIF_12_FONT_ID);
      assert(id!=NOTOSANS_14_FONT_ID && id!=NOTOSERIF_14_FONT_ID);
    } else assert(id==(family==1?NOTOSANS_12_FONT_ID:NOTOSERIF_12_FONT_ID));
    assert(settings.fontPointSize==size);
  }
  settings.sdFontFamilyName[0]='A';
  settings.sdFontIdResolver=[](void*,const char*,uint8_t) { return 123456; };
  assert(settings.getReaderFontId()==123456);
}
'''
        for defines in ((), ('CROSSMUX_UI_PROFILE_HIGH_DPI',)):
            run_cpp(program, include_dirs=(ROOT / 'src',), defines=defines)

    def test_all_main_tabs_share_drawing_and_input_geometry(self):
        activity = (ROOT / 'src/activities/Activity.cpp').read_text()
        manager = (ROOT / 'src/activities/ActivityManager.cpp').read_text()
        program = r'''
#include <cassert>
#include <utility>
#include "components/SubpageLayout.h"
#include "components/themes/inx/InxTheme.h"
class GfxRenderer {
 public:
  int getScreenWidth() const { return 1300; }
  int getScreenHeight() const { return 1300; }
  void getOrientedViewableTRBL(int* t,int* r,int* b,int* l) const { *t=*r=*b=*l=0; }
};
struct UITheme {
  bool tabs=true;
  Rect safe{}, drawn{};
  ThemeMetrics metrics=InxMetrics::values;
  MainTab selected=MainTab::None;
  int headers=0;
  static UITheme& getInstance() { static UITheme theme; return theme; }
  bool hasMainTabs() const { return tabs; }
  UITheme& getTheme() { return *this; }
  const ThemeMetrics& getMetrics() const { return metrics; }
  Rect getScreenSafeArea(const GfxRenderer&,bool,bool=false) { return safe; }
  void drawMainTabStatusBar(const GfxRenderer&,Rect) {}
  void drawMainTabBar(const GfxRenderer&,Rect rect,MainTab tab) { drawn=rect; selected=tab; }
  void drawHeader(const GfxRenderer&,Rect rect,const char*,const char*) { drawn=rect; ++headers; }
};
#define GUI UITheme::getInstance().getTheme()
struct Activity {
  GfxRenderer renderer;
  MainTab tab=MainTab::Recent;
  bool bottom=false;
  struct { bool hasTouch() const { return true; } } mappedInput;
  bool usesMainTabBar() const;
  bool mainTabsAtBottom() const { return bottom; }
  bool hasMainTabStatusBar() const { return usesMainTabBar() && bottom; }
  MainTabLayout mainTabLayout() const;
  MainTab mainTab() const { return tab; }
  bool mainTabBackReturnsToTabs() const { return false; }
  void selectMainTabContentEdge(MainTabContentEdge) {}
  void drawPageHeader(const Rect&,const char*,const char* =nullptr) const;
};
struct MappedInputManager {
  enum class Button { None,Left,Right,Up,Down,Confirm,Back };
  bool tapped=true,down=false;
  int x=0,y=0;
  Button released=Button::None;
  bool wasScreenTapped(int& tx,int& ty) { tx=x;ty=y;return tapped; }
  bool wasScreenTouchDown(int& tx,int& ty) { tx=x;ty=y;return down; }
  bool wasReleased(Button b) { return b==released; }
  bool wasPressed(Button) { return false; }
  bool isPressed(Button) { return false; }
};
struct ActivityManager {
  Activity* currentActivity=nullptr;
  GfxRenderer renderer;
  MappedInputManager mappedInput;
  MainTabFocus mainTabFocus=MainTabFocus::Content;
  bool mainTabEntryReleasePending=false;
  MainTab destination=MainTab::None;
  int updates=0;
  void goToMainTab(MainTab tab) { destination=tab; }
  void requestUpdate() { ++updates; }
  void goToStandby() { assert(false); }
  bool handleMainTabInput();
};
''' + method(activity, 'bool Activity::usesMainTabBar(') + method(activity, 'MainTabLayout Activity::mainTabLayout(') + method(activity, 'void Activity::drawPageHeader(') + method(manager, 'bool ActivityManager::handleMainTabInput(') + r'''
int main() {
  auto& theme=UITheme::getInstance();
  for (const Rect safe : {Rect{5,5,674,1203},Rect{8,5,1203,674},Rect{5,8,674,1203},Rect{5,5,1203,674},Rect{0,0,480,800}}) {
    theme.safe=safe;
    for (bool bottom : {false,true}) for (MainTab current : MainTabs::values) {
      Activity page; page.tab=current; page.bottom=bottom;
      const Rect expected=page.mainTabLayout().tabBar;
      page.drawPageHeader(Rect{0,0,999,66},"title");
      assert(theme.drawn.x==expected.x && theme.drawn.y==expected.y);
      assert(theme.drawn.width==expected.width && theme.drawn.height==expected.height && theme.selected==current);
      for (int i=0;i<5;++i) {
        const int left=expected.x+expected.width*i/5, right=expected.x+expected.width*(i+1)/5;
        for (int x=left;x<right;++x) {
          ActivityManager m; m.currentActivity=&page;
          m.mappedInput.x=x; m.mappedInput.y=expected.y;
          assert(m.handleMainTabInput());
          const MainTab target=MainTabs::fromX(x-expected.x,expected.width);
          assert(target==MainTab::None ? m.updates==0 && m.destination==MainTab::None
                 : current==target ? m.updates==1 && m.destination==MainTab::None : m.destination==target);
        }
      }
      for (auto point : {std::pair{expected.x-1,expected.y},std::pair{expected.x+expected.width,expected.y},
                         std::pair{expected.x,expected.y-1},std::pair{expected.x,expected.y+expected.height}}) {
        ActivityManager m; m.currentActivity=&page; m.mainTabFocus=MainTabFocus::Tabs;
        m.mappedInput.x=point.first; m.mappedInput.y=point.second;
        assert(!m.handleMainTabInput() && m.destination==MainTab::None && m.mainTabFocus==MainTabFocus::Content);
        m.mainTabFocus=MainTabFocus::Tabs; m.mappedInput.tapped=false; m.mappedInput.down=true;
        assert(!m.handleMainTabInput() && m.mainTabFocus==MainTabFocus::Content);
      }
      for (auto button : {MappedInputManager::Button::Left,MappedInputManager::Button::Right}) {
        ActivityManager m; m.currentActivity=&page; m.mainTabFocus=MainTabFocus::Tabs;
        m.mappedInput.tapped=false; m.mappedInput.released=button;
        assert(m.handleMainTabInput());
        assert(m.destination==MainTabs::adjacent(current,button==MappedInputManager::Button::Left ? -1 : 1));
      }
    }
  }
  theme.tabs=false;
  Activity page; const Rect legacy{0,9,684,66}; page.drawPageHeader(legacy,"legacy","version");
  assert(theme.headers==1 && theme.drawn.x==0 && theme.drawn.y==9 && theme.drawn.width==684);
  ActivityManager m; m.currentActivity=&page; assert(!m.handleMainTabInput());
  theme.tabs=true; page.tab=MainTab::None; page.drawPageHeader(legacy,"picker");
  assert(theme.headers==2 && !m.handleMainTabInput());
}
'''
        run_cpp(program, include_dirs=(ROOT / 'src', ROOT / 'lib/hal'))

    def test_home_standby_owns_a_complete_back_gesture_across_transitions(self):
        source = (ROOT / 'src/activities/ActivityManager.cpp').read_text()
        header = (ROOT / 'src/activities/ActivityManager.h').read_text()
        state_start = header.index('  enum class StandbyBackState')
        state_end = header.index(';', header.index('  StandbyBackState standbyBackState', state_start)) + 1
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "activities/MainTab.h"
#include "components/HeaderBackTapTarget.h"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
enum { eIncrement };
void xTaskNotify(int,int,int) {}
struct { void clearTouchTapEvent() {} } gpio;
struct RenderLock {
  enum class Mode { Block,Try };
  static inline bool busy=false;
  RenderLock() = default;
  explicit RenderLock(Mode) {}
  static bool peek() { return busy; }
  bool ownsLock() const { return false; }
  void unlock() {}
};
[[maybe_unused]] static struct { void railsOffIfIdle(uint32_t) {} } display;
struct ActivityResult {};
struct MappedInputManager {
  enum class Button { None,Back,Confirm,Left,Right,Up,Down };
  bool pressed=false,released=false,held=false,suppressed=false;
  bool tap=false,down=false,home=false,light=false;
  int x=0,y=0;
  Button otherPress=Button::None,otherRelease=Button::None;
  bool wasPressed(Button b) const { return b==Button::Back ? pressed : b==otherPress; }
  bool wasReleased(Button b) const { return b==Button::Back ? released : b==otherRelease; }
  bool isPressed(Button b) const { return b==Button::Back && held; }
  bool consumeSuppressedRelease() { return std::exchange(suppressed,false); }
  bool wasScreenTapped(int& tx,int& ty) const { tx=x;ty=y;return tap; }
  bool wasScreenTouchDown(int& tx,int& ty) const { tx=x;ty=y;return down; }
  bool wasHomeGesture() const { return home; }
  bool backGesture=false;  // last, so the designated initializers above keep their order
  bool wasBackGesture() const { return backGesture; }
  bool wasLightPanelGesture() const { return light; }
  bool hasTouch() const { return true; }
  void resetHomeButtonInput() {}
};
struct Activity {
  std::string name="Home";
  bool home=true,tabs=false,exclusive=false;
  MainTab tab=MainTab::Recent;
  ActivityResult result;
  std::function<void(ActivityResult)> resultHandler;
  std::function<void()> action;
  int loops=0;
  virtual ~Activity()=default;
  void onEnter() {}
  void onExit() {}
  void loop() { ++loops; if(action) action(); }
  bool requiresExclusiveStorageLoop() const { return exclusive; }
  bool isHomeActivity() const { return home; }
  bool handleHomeGesture() const { return false; }
  bool usesMainTabBar() const { return tabs; }
  MainTab mainTab() const { return tab; }
  MainTabLayout mainTabLayout() const { return {Rect{0,700,480,56},Rect{0,0,480,28},Rect{0,34,480,650}}; }
  bool mainTabBackReturnsToTabs() const { return true; }
  void selectMainTabContentEdge(MainTabContentEdge) {}
};
std::unique_ptr<Activity> page(const char* theme) {
  auto p=std::make_unique<Activity>();
  p->tabs=std::string(theme)=="INX"; p->home=!p->tabs;
  p->name=p->tabs?"InxRecent":"Home";
  return p;
}
struct FrontlightPanelActivity : Activity {
  FrontlightPanelActivity(int,MappedInputManager&,bool=false) { home=false;name="FrontlightPanel"; }
};
template<class T,class... Args> auto makeUniqueNoThrow(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}
struct { bool standbyShortcutEnabled=true; int rickyGestureStatus=1; } SETTINGS;
struct ActivityManager {
''' + header[state_start:state_end] + r'''
  enum class PendingAction { None,Push,Pop,Replace };
  std::atomic<PendingAction> pendingAction{PendingAction::None};
  std::atomic<bool> requestedUpdate{false};
  std::unique_ptr<Activity> currentActivity,pendingActivity;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  MappedInputManager mappedInput;
  MainTabFocus mainTabFocus=MainTabFocus::Tabs;
  bool mainTabEntryReleasePending=false;
  int renderer=0,renderTaskHandle=0,standbyCalls=0;
  const char* theme;
  explicit ActivityManager(const char* value):theme(value) { replaceActivity(page(theme)); }
  void requestUpdate() { requestedUpdate=true; }
  void cancelIdleRender() {}
  void goHome() { mainTabFocus=MainTabFocus::Tabs;replaceActivity(page(theme)); }
  void goToMainTab(MainTab tab) {
    auto p=page("INX");p->tab=tab;replaceActivity(std::move(p));
  }
  bool standbyShowing() const { return currentActivity && currentActivity->name == "Standby"; }
  void goToStandby() {
    ++standbyCalls;
    auto p=std::make_unique<Activity>();p->home=false;p->name="Standby";
    replaceActivity(std::move(p));
  }
  void resetHomeStandbyInput();
  bool handleHomeStandbyInput();
  bool handleMainTabInput();
  void replaceActivity(std::unique_ptr<Activity>&&);
  void pushActivity(std::unique_ptr<Activity>&&);
  void popActivity();
  void exitActivity(const RenderLock&);
  void loop();
  void tick(MappedInputManager input={}) {
    mappedInput=input;loop();
    if(pendingAction.load()!=PendingAction::None && !RenderLock::busy) {
      mappedInput={.held=input.held};loop();
    }
  }
};
''' + '\n'.join(method(source, name) for name in (
            'void ActivityManager::resetHomeStandbyInput(',
            'bool ActivityManager::handleHomeStandbyInput(',
            'bool ActivityManager::handleMainTabInput(',
            'void ActivityManager::replaceActivity(',
            'void ActivityManager::pushActivity(',
            'void ActivityManager::popActivity(',
            'void ActivityManager::exitActivity(',
            'void ActivityManager::loop(')) + r'''
void freshBack(ActivityManager& m) {
  m.tick({.pressed=true,.held=true});assert(m.standbyCalls==0);
  m.tick({.held=true});assert(m.standbyCalls==0);
  m.tick({.released=true});assert(m.standbyCalls==1);
  m.tick();assert(m.standbyCalls==1 && m.currentActivity->name=="Standby");
}
int main() {
  using Button=MappedInputManager::Button;
  for(const char* theme:{"INX","Classic","Carousel","Cover Grid"}) {
    ActivityManager stray(theme);
    stray.tick({.released=true});assert(stray.standbyCalls==0);
    freshBack(stray);

    // Both the queued request and actual activation cancel the old pair.
    ActivityManager replace(theme);
    replace.tick({.pressed=true,.held=true});
    auto* outgoing=replace.currentActivity.get();
    replace.replaceActivity(page(theme));
    RenderLock::busy=true;
    replace.tick({.held=true});assert(replace.currentActivity.get()==outgoing);
    RenderLock::busy=false;
    replace.tick({.pressed=true,.held=true}); // old owner's press edge at activation
    replace.tick({.released=true});assert(replace.standbyCalls==0);
    freshBack(replace);

    // Push a panel while armed, then return to the very same parent on press.
    ActivityManager pop(theme);
    auto* parent=pop.currentActivity.get();
    pop.tick({.pressed=true,.held=true});
    pop.pushActivity(std::make_unique<FrontlightPanelActivity>(0,pop.mappedInput));
    pop.tick({.held=true});assert(pop.stackActivities.back().get()==parent);
    pop.popActivity();pop.tick({.pressed=true,.held=true});
    assert(pop.currentActivity.get()==parent);
    pop.tick({.released=true});assert(pop.standbyCalls==0);
    freshBack(pop);

    // Crash report exits on press with an empty activity stack.
    ActivityManager crash(theme);
    crash.currentActivity->home=false;crash.currentActivity->tabs=false;
    crash.currentActivity->name="Crash";
    crash.currentActivity->action=[&crash] {
      if(crash.mappedInput.wasPressed(Button::Back)) crash.popActivity();
    };
    crash.tick({.pressed=true,.held=true});
    crash.tick({.released=true});assert(crash.standbyCalls==0);
    freshBack(crash);

    ActivityManager suppressed(theme);
    suppressed.tick({.pressed=true,.held=true});
    suppressed.tick({.released=true,.suppressed=true});
    suppressed.tick({.released=true});assert(suppressed.standbyCalls==0);
    freshBack(suppressed);

    ActivityManager disabled(theme);
    disabled.tick({.pressed=true,.held=true});
    SETTINGS.standbyShortcutEnabled=false;
    disabled.tick({.held=true});
    SETTINGS.standbyShortcutEnabled=true;
    disabled.tick({.released=true});assert(disabled.standbyCalls==0);
    SETTINGS.standbyShortcutEnabled=false;
    disabled.tick({.pressed=true,.released=true});assert(disabled.standbyCalls==0);
    SETTINGS.standbyShortcutEnabled=true;freshBack(disabled);

    // A completed touch Back remains independent of an inherited button hold.
    ActivityManager gesture(theme);
    gesture.mappedInput={.held=true};gesture.resetHomeStandbyInput();
    gesture.tick({.pressed=true,.released=true,.held=true});assert(gesture.standbyCalls==1);

#ifdef RICKYOS_PRODUCT
    // RickyOS touch screens: the Back swipe turns Home pages and never opens Standby;
    // the Back key (the ring under the screen) still does.
    ActivityManager swipe(theme);
    swipe.tick({.pressed=true,.released=true,.backGesture=true});assert(swipe.standbyCalls==0);
    ActivityManager key(theme);
    key.tick({.pressed=true,.held=true});
    key.tick({.released=true});assert(key.standbyCalls==1);
#endif

    // A held-key barrier cannot prevent touch opening a book or control center.
    for(bool releaseWithTap:{false,true}) {
      ActivityManager book(theme);
      book.mappedInput={.held=true};book.resetHomeStandbyInput();
      book.currentActivity->action=[&book] {
        if(book.mappedInput.tap) {
          auto p=std::make_unique<Activity>();p->home=false;p->name="Reader";
          book.replaceActivity(std::move(p));
        }
      };
      book.tick({.released=releaseWithTap,.held=!releaseWithTap,.tap=true,.x=100,.y=200});
      assert(book.currentActivity->name=="Reader" && book.standbyCalls==0);
    }
    ActivityManager panel(theme);
    panel.mappedInput={.held=true};panel.resetHomeStandbyInput();
    panel.tick({.held=true,.tap=true,.x=100,.y=10});
    assert(panel.currentActivity->name=="FrontlightPanel" && panel.standbyCalls==0);
    panel.popActivity();panel.tick({.held=true});
    panel.tick({.released=true});assert(panel.standbyCalls==0);
    // INX status-bar tap moves focus into content, so another Back returns to tabs.
    if(panel.mainTabFocus==MainTabFocus::Content) panel.tick({.released=true});
    freshBack(panel);
  }

  ActivityManager focus("INX");
  focus.tick({.pressed=true,.held=true});
  focus.tick({.held=true,.otherRelease=Button::Confirm});
  assert(focus.mainTabFocus==MainTabFocus::Content);
  focus.tick({.released=true});
  assert(focus.mainTabFocus==MainTabFocus::Tabs && focus.standbyCalls==0);
  focus.tick({.released=true});assert(focus.standbyCalls==0);
  freshBack(focus);

  for(MainTab tab:MainTabs::values) {
    if(tab==MainTab::Recent) continue;
    ActivityManager tabs("INX");tabs.currentActivity->tab=tab;
    tabs.tick({.released=true});
    assert(tabs.currentActivity->tab==MainTab::Recent && tabs.standbyCalls==0);
    tabs.tick({.released=true});assert(tabs.standbyCalls==0);
    freshBack(tabs);
  }
  ActivityManager touchTab("INX");
  touchTab.mappedInput={.held=true};touchTab.resetHomeStandbyInput();
  touchTab.tick({.held=true,.tap=true,.x=144,.y=720});
  assert(touchTab.currentActivity->tab==MainTab::Library && touchTab.standbyCalls==0);
}
'''
        for defines in ((), ('CROSSMUX_UI_PROFILE_HIGH_DPI', 'FREEINK_DEVICE_READPICO=1'),
                        ('RICKYOS_PRODUCT=1', 'CROSSMUX_UI_PROFILE_HIGH_DPI', 'FREEINK_DEVICE_READPICO=1')):
            run_cpp(program, include_dirs=(ROOT / 'src',), defines=defines)

    def test_readpico_safe_area_in_all_orientations(self):
        board = (ROOT / 'freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h').read_text()
        profile = method(board, 'constexpr BoardProfile READ_PICO =')
        match = re.search(r'\{(\d+), (\d+), (\d+), (\d+)\},\s*// portrait TRBL', profile)
        self.assertIsNotNone(match)
        insets = tuple(map(int, match.groups()))
        self.assertEqual(insets, (5, 5, 8, 5))
        renderer = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        theme = (ROOT / 'src/components/UITheme.cpp').read_text()
        run_cpp(r'''
#include <cassert>
#include <initializer_list>
#define FREEINK_DEVICE_READPICO 1
namespace BoardConfig {
struct Insets { int top, right, bottom, left; };
struct Profile { Insets viewableInsets; };
constexpr Profile ACTIVE{{INSETS}};
}
struct GfxRenderer {
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation orientation=Portrait;
  static constexpr int VIEWABLE_MARGIN_TOP=9, VIEWABLE_MARGIN_RIGHT=3,
                       VIEWABLE_MARGIN_BOTTOM=3, VIEWABLE_MARGIN_LEFT=3;
  Orientation getOrientation() const { return orientation; }
  bool portrait() const { return orientation==Portrait || orientation==PortraitInverted; }
  int getScreenWidth() const { return portrait() ? 684 : 1216; }
  int getScreenHeight() const { return portrait() ? 1216 : 684; }
  void getOrientedViewableTRBL(int*,int*,int*,int*) const;
};
struct Rect { int x,y,width,height; };
struct ThemeMetrics { int buttonHintsHeight=0; };
struct UITheme {
  ThemeMetrics getMetrics() const { return {}; }
  Rect getScreenSafeArea(const GfxRenderer&,bool,bool);
};
'''.replace('INSETS', ','.join(map(str, insets))) +
                method(renderer, 'void GfxRenderer::getOrientedViewableTRBL(') +
                method(theme, 'Rect UITheme::getScreenSafeArea(').replace(
                    'bool hasSideButtonHints', '[[maybe_unused]] bool hasSideButtonHints') + r'''
int main() {
  GfxRenderer renderer;
  UITheme theme;
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
    renderer.orientation=orientation;
    int top=0, right=0, bottom=0, left=0;
    renderer.getOrientedViewableTRBL(&top,&right,&bottom,&left);
    const BoardConfig::Insets expected[]={{5,5,8,5},{5,5,5,8},{8,5,5,5},{5,8,5,5}};
    const auto inset=expected[orientation];
    assert(top==inset.top && right==inset.right && bottom==inset.bottom && left==inset.left);
    const Rect safe=theme.getScreenSafeArea(renderer,false,false);
    assert(safe.x==inset.left && safe.y==inset.top);
    assert(safe.width==renderer.getScreenWidth()-inset.left-inset.right && safe.height==renderer.getScreenHeight()-inset.top-inset.bottom);
    const Rect hiddenHints=theme.getScreenSafeArea(renderer,true,false);
    assert(hiddenHints.x==safe.x && hiddenHints.y==safe.y);
    assert(hiddenHints.width==safe.width && hiddenHints.height==safe.height);
  }
}
''')

    def test_sync_refresh_survives_wifi_child(self):
        source = (WEREAD / 'WeReadProgressSyncActivity.cpp').read_text()
        enter = method(source, 'void WeReadProgressSyncActivity::onEnter(')
        callback = method(source, 'void WeReadProgressSyncActivity::onWifiSelectionComplete(')
        render = method(source, 'void WeReadProgressSyncActivity::render(')
        display = render[render.rindex('  renderer.displayBuffer('):render.rindex('}')]
        run_cpp(r'''
#include <atomic>
#include <cassert>
#include <initializer_list>
namespace HalDisplay { enum RefreshMode { FULL_REFRESH, FAST_REFRESH }; }
struct Renderer {
  HalDisplay::RefreshMode last = HalDisplay::FAST_REFRESH;
  void displayBuffer(HalDisplay::RefreshMode mode=HalDisplay::FAST_REFRESH) { last=mode; }
};
struct Activity { void onEnter() {} };
namespace ReaderUtils { void applyOrientation(Renderer&, int) {} }
struct { int orientation=0; } SETTINGS;
bool loggedIn=true;
namespace WeReadStore {
struct Session { bool valid() { return true; } void clear() {} };
bool loadSession(Session&) { return loggedIn; }
}
namespace NetworkStartup { void prepare(Renderer&) {} }
constexpr int WL_CONNECTED=1;
struct { int connected=1; int status() { return connected; } } WiFi;
struct WeReadProgressSyncActivity : Activity {
  enum class State { WifiSelection, Starting, LoginRequired };
  State state_ = State::WifiSelection;
  std::atomic<bool> fullRefreshPending_{true};
  Renderer renderer;
  bool wifiActivated_=false, returned=false, child=false;
  void requestUpdate() {}
  void launchWifiSelection() { child=true; }
  void returnToReader() { returned=true; }
  void onEnter();
  void onWifiSelectionComplete(bool);
  void renderRefresh() { DISPLAY }
};
'''.replace('DISPLAY', display) + enter + callback + r'''
int main() {
  for (bool login : {false,true}) for (int connected : {0,1}) {
    loggedIn=login; WiFi.connected=connected;
    WeReadProgressSyncActivity page;
    page.onEnter();
    if (page.child) {
      page.renderer.displayBuffer(); // A child paint cannot consume the parent's flag.
      WiFi.connected=1;
      page.onWifiSelectionComplete(true);
    }
    page.renderRefresh(); assert(page.renderer.last==HalDisplay::FULL_REFRESH);
    page.renderRefresh(); assert(page.renderer.last==HalDisplay::FAST_REFRESH);
    WiFi.connected=1;
    page.onWifiSelectionComplete(true);
    page.renderRefresh(); assert(page.renderer.last==HalDisplay::FULL_REFRESH);
    page.renderRefresh(); assert(page.renderer.last==HalDisplay::FAST_REFRESH);
    page.onWifiSelectionComplete(false); assert(page.returned);
  }
}
''')

    def test_resource_error_messages_and_layout(self):
        source = (WEREAD / 'WeReadActivity.cpp').read_text()
        message = method(source, 'const char* WeReadActivity::errorMessage(')
        render = method(source, 'void WeReadActivity::render(')
        error = render.split('    case State::Error: {', 1)[1].split('    case State::LogoutError:', 1)[0]
        keys = ('STR_WEREAD_STORAGE_ERROR', 'STR_WEREAD_CHECK_STORAGE_SPACE', 'STR_WEREAD_CHECK_SD_CARD',
                'STR_WEREAD_MEMORY_ERROR', 'STR_WEREAD_RESTART_HINT', 'STR_WEREAD_HTTP_ERROR',
                'STR_WEREAD_NO_WIFI', 'STR_WEREAD_CACHE_NOT_AVAILABLE', 'STR_WEREAD_CACHE_WHOLE_BOOK_ONLY')
        for language in ('chinese', 'english'):
            spec = importlib.util.spec_from_file_location('gen_i18n', ROOT / 'scripts/gen_i18n.py')
            generator = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(generator)
            translations = generator.parse_yaml_file(str(ROOT / f'lib/I18n/translations/{language}.yaml'))
            strings = '\n'.join(f'const char* {key} = {json.dumps(translations[key], ensure_ascii=False)};' for key in keys)
            run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <initializer_list>
''' + strings + r'''
#define tr(key) key
namespace WeReadClient { enum class Error { SdCard, OutOfMemory, Network, Unavailable, WholeBookOnly, Protocol }; }
namespace EpdFontFamily { enum Style { BOLD, REGULAR }; }
constexpr int UI_10_FONT_ID=10, WL_CONNECTED=1;
struct { int connected=1; int status() const { return connected; } } WiFi;
struct Rect { int x,y,width,height; };
struct Metrics { int contentSidePadding=20; };
struct Renderer {
  struct ClipScope { ClipScope(const Renderer&,int,int,int,int) {} };
  int getLineHeight(int) const { return 26; }
  int getTextWidth(const char* text) const {
    int width=0;
    for (; *text; ++text) {
      unsigned char c=*text;
      if (c < 128) width+=12;
      else if ((c & 0xc0) != 0x80) width+=26;
    }
    return width;
  }
};
using GfxRenderer=Renderer;
std::vector<std::string> shown;
struct { void drawPopup(const Renderer&, const char* s) { shown.emplace_back(s); } } GUI;
namespace SubpageLayout {
int sectionGap(const Metrics&) { return 12; }
int centeredTop(Rect r,int h) { return r.y + std::max(0,(r.height-h)/2); }
Rect insetHorizontal(Rect r,int n) { return {r.x+n,r.y,r.width-2*n,r.height}; }
}
namespace UITheme {
void drawCenteredText(const Renderer& r,Rect rect,int,int y,const char* text,bool,EpdFontFamily::Style) {
  assert(r.getTextWidth(text)<=rect.width);
  assert(y>=rect.y && y+26<=rect.y+rect.height);
  shown.emplace_back(text);
}
}
struct WeReadActivity {
  WeReadClient::Error error_;
  Renderer renderer;
  Metrics metrics;
  Rect content;
  enum class State { Error };
  const char* errorMessage() const;
  void renderError() { switch (State::Error) { case State::Error: { ERROR } }
};
'''.replace('ERROR', error) + message + r'''
int main() {
  using E=WeReadClient::Error;
  for (Rect bounds : {Rect{0,66,480,694},Rect{0,66,800,374}}) {
    WeReadActivity page{E::SdCard,{}, {},bounds};
    shown.clear(); page.renderError();
    assert(shown==std::vector<std::string>({STR_WEREAD_STORAGE_ERROR,STR_WEREAD_CHECK_STORAGE_SPACE,STR_WEREAD_CHECK_SD_CARD}));
    page.error_=E::OutOfMemory; shown.clear(); page.renderError();
    assert(shown==std::vector<std::string>({STR_WEREAD_MEMORY_ERROR,STR_WEREAD_RESTART_HINT}));
    page.error_=E::Network; shown.clear(); page.renderError();
    assert(shown==std::vector<std::string>({STR_WEREAD_HTTP_ERROR}));
    WiFi.connected=0; shown.clear(); page.renderError();
    assert(shown==std::vector<std::string>({STR_WEREAD_NO_WIFI})); WiFi.connected=1;
  }
}
''')


    def test_cover_download_creates_directory_before_request(self):
        source = (ROOT / 'lib/WeReadWebApi/src/WeReadClient.cpp').read_text()
        download = method(source, 'Error Operation::fetchCoverSource(')
        sink = source[source.index('struct FileSink {'):source.index('bool finishFile(')]
        run_cpp(r"""
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
std::string lastLog;
void logError(const char*, const char* format, ...) {
  char buf[512]; va_list args; va_start(args,format);
  vsnprintf(buf,sizeof(buf),format,args); va_end(args); lastLog=buf;
}
#define LOG_ERR(...) logError(__VA_ARGS__)
bool shortWrite=false;
struct HalFile {
  std::ofstream stream;
  bool isOpen() const { return stream.is_open(); }
  void close() { stream.close(); }
  size_t write(const void* data,size_t size) {
    if (shortWrite) return 0;
    stream.write(static_cast<const char*>(data),size);
    return stream.good() ? size : 0;
  }
};
struct {
  int directoryChecks=0;
  bool exists(const char* p) { return fs::exists(p); }
  bool remove(const char* p) { return fs::remove(p); }
  bool ensureDirectoryExists(const char* p) {
    ++directoryChecks;
    std::error_code ec;
    fs::create_directories(p,ec);
    return !ec && fs::is_directory(p);
  }
  bool openFileForWrite(const char*,const std::string& path,HalFile& file) {
    file.stream.open(path,std::ios::binary|std::ios::trunc);
    return file.isOpen();
  }
} Storage;
namespace WeReadProtocol { enum class ImageType { None,Jpeg,Png,Detect }; }
namespace WeReadStore {
  bool rootReady=true;
  bool ensureRoot() { return rootReady; }
  struct ImageRecord { char href[64]{},url[512]{}; };
  enum class ImageWorkState { Pending,Skipped,Complete };
}
namespace WeReadHttpClient {
  bool extractHttpsHost(const char* url,char*,size_t) { return strncmp(url,"https://",8)==0; }
}
const char* coverSourceName(WeReadProtocol::ImageType type) {
  return type==WeReadProtocol::ImageType::Png ? "cover.png" : "cover.jpg";
}
""" + sink + r"""
enum class Error { Ok,SdCard,Protocol };
enum class CoverWorkResult { Skipped,Pending,Complete };
struct Operation {
  WeReadProtocol::ImageType coverType_=WeReadProtocol::ImageType::Jpeg;
  WeReadStore::ImageWorkState coverState_=WeReadStore::ImageWorkState::Pending;
  char url_[512]="https://cdn.weread.qq.com/cover.jpg",imageHost_[128]{};
  std::string bookDir_;
  uint8_t coverAttempts_=0,coverRedirects_=0;
  int requests=0;
  Error fetchCoverSource(CoverWorkResult&);
  Error requestImage(WeReadStore::ImageRecord& image,WeReadStore::ImageWorkState& state,
                     uint8_t&,uint8_t&,bool,WeReadProtocol::ImageType* detected) {
    ++requests;
    assert(fs::is_directory(bookDir_)); // Original code fails here for a shelf-only book.
    const std::string path=bookDir_+"/"+image.href+".part";
    FileSink sink; sink.path=&path;
    const uint8_t data[]={0xff,0xd8,0xff};
    if (!resetFile(&sink) || !writeFile(&sink,data,sizeof(data))) return Error::SdCard;
    state=WeReadStore::ImageWorkState::Complete;
    if (detected) *detected=WeReadProtocol::ImageType::Jpeg;
    return Error::Ok;
  }
};
""" + download + r"""
int main(int argc,char** argv) {
  assert(argc==1);
  const fs::path root=fs::path(argv[0]).parent_path()/"sd";
  Operation op; op.bookDir_=(root/"weread"/"new-book").string();
  CoverWorkResult result;
  assert(!fs::exists(op.bookDir_));
  assert(op.fetchCoverSource(result)==Error::Ok && result==CoverWorkResult::Complete);
  assert(fs::file_size(fs::path(op.bookDir_)/"cover.jpg.part")==3);
  assert(op.fetchCoverSource(result)==Error::Ok && op.requests==2);
  WeReadStore::rootReady=false;
  assert(op.fetchCoverSource(result)==Error::SdCard && op.requests==2);
  assert(lastLog.find(op.bookDir_)!=std::string::npos);
  WeReadStore::rootReady=true;
  op.bookDir_=(root/"not-a-directory").string(); std::ofstream(op.bookDir_) << "keep";
  assert(op.fetchCoverSource(result)==Error::SdCard && op.requests==2);
  assert(fs::file_size(op.bookDir_)==4);
  const int checks=Storage.directoryChecks;
  for (const char* url : {"", "http://invalid/cover.jpg"}) {
    strcpy(op.url_,url);
    assert(op.fetchCoverSource(result)==Error::Ok && result==CoverWorkResult::Skipped);
  }
  assert(Storage.directoryChecks==checks && op.requests==2);
  std::string missing=(root/"missing"/"cover.part").string(); FileSink failed; failed.path=&missing;
  assert(!resetFile(&failed) && lastLog.find(missing)!=std::string::npos);
  std::string good=(root/"short.part").string(); FileSink shortSink; shortSink.path=&good;
  assert(resetFile(&shortSink)); shortWrite=true;
  const uint8_t bytes[]={1,2,3};
  assert(!writeFile(&shortSink,bytes,3));
  assert(shortSink.failure==FileSink::Failure::SdCard && shortSink.size==0);
  assert(lastLog.find("written=0 expected=3")!=std::string::npos);
}
""")


    def test_txt_spacing_keeps_cache_fields_byte_aligned(self):
        import re
        # Immutable legacy schema fixture; active TXT books now use EPUB sections.
        source = subprocess.check_output(['git', '-C', str(ROOT), 'show',
            '593c8dbc8feb404d740108a3db281c4eeae3e33e:src/activities/reader/TxtReaderActivity.cpp'], text=True)
        read = method(source, 'bool readPodChecked(')
        write = method(source, 'bool writePodChecked(')
        expression = re.search(
            r'!writePodChecked\(f, (.*SETTINGS\.extraParagraphSpacing.*?)\)\s*\|\|\s*!writePodChecked\(f, complete\)',
            source).group(1)
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
struct HalFile {
  std::vector<uint8_t> bytes;
  size_t position=0;
  size_t write(const uint8_t* data,size_t size) {bytes.insert(bytes.end(),data,data+size);return size;}
  int read(uint8_t* data,size_t size) {
    if(position+size>bytes.size()) return 0;
    std::memcpy(data,bytes.data()+position,size);position+=size;return static_cast<int>(size);
  }
};
struct {uint8_t extraParagraphSpacing=0;} SETTINGS;
''' + 'template<typename T>\n' + read + '\ntemplate<typename T>\n' + write + r'''
int main() {
  for(uint8_t level=0;level<=5;++level) {
    SETTINGS.extraParagraphSpacing=level;
    HalFile f;
    assert(writePodChecked(f, ''' + expression + r'''));
    const uint8_t complete=1, encoding=1;
    const uint32_t pageCount=7;
    assert(writePodChecked(f,complete));assert(writePodChecked(f,encoding));assert(writePodChecked(f,pageCount));
    assert(f.bytes.size()==7);
    HalFile reopened=f;
    uint8_t spacing=255,loadedComplete=0,loadedEncoding=0;uint32_t loadedPages=0;
    assert(readPodChecked(reopened,spacing));assert(readPodChecked(reopened,loadedComplete));
    assert(readPodChecked(reopened,loadedEncoding));assert(readPodChecked(reopened,loadedPages));
    assert(spacing==(level!=0) && loadedComplete==1 && loadedEncoding==1 && loadedPages==7);
    assert(reopened.position==reopened.bytes.size());
  }
}
''')


if __name__ == '__main__':
    unittest.main()
