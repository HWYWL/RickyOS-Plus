"""Product reading chrome retains the existing reader settings and pipeline."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyReaderChromeTest(unittest.TestCase):
    def test_actual_chrome_is_bounded_in_portrait_landscape_and_respects_visibility(self):
        source = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "components/RickyDesignTokens.h"
#include "activities/reader/ReaderChrome.h"
struct Rect {int x,y,width,height;};
namespace freeink::ui {enum class TextAlign {Left,Center,Right};}
class GfxRenderer {
 public:
  int w=684,h=1216,lineHeight=34;
  struct ClipScope {ClipScope(GfxRenderer&,int,int,int,int) {}};
  int getTextWidth(int,const char* text) {return std::strlen(text)*12;}
  int getLineHeight(int) const {return lineHeight;}
  void drawText(int,int x,int y,const char*) {assert(x>=0 && x<w && y>=0 && y<h);}
};
struct Spec {
  bool lane=true,showBattery=true,showBatteryPercent=true,clock=true;
  bool showChapterPageCount=true,showBookProgressPercent=true,bar=true;
  int clockMode=0,progressBarHeightPx=6,progressBarMode=0;
  bool clock12h=false;
  bool textLaneVisible() const {return lane;}
  bool headerLaneVisible() const {return lane;}
  bool footerLaneVisible() const {return lane && (showChapterPageCount || showBookProgressPercent);}
  bool showsClock() const {return clock;}
  bool showsProgressBar() const {return bar;}
};
struct CrossPointSettings {
  enum {STATUS_BAR_CLOCK_RIGHT=1};
  enum STATUS_BAR_PROGRESS_BAR {BOOK_PROGRESS=0};
  Spec spec;
  Spec statusBarSpec() const {return spec;}
} settings;
#define SETTINGS settings
constexpr int batteryPercentSpacing=8,STATUS_NUMERIC_FONT_ID=1;
constexpr int bluetoothStatusIconWidth=24,bookmarkStatusIconWidth=24;
struct Power {unsigned getBatteryPercentage() {return 100;}} powerManager;
namespace bleinput {bool connected=false; bool isConnected() {return connected;}}
namespace TimeUtils {bool formatCurrentTime(char* text,size_t n,bool) {std::snprintf(text,n,"08:25");return true;}}
struct UITheme {
  static UITheme& getInstance() {static UITheme t;return t;}
  Rect getScreenSafeArea(GfxRenderer& r) {return {5,5,r.w-10,r.h-13};}
};
namespace RickyDesignUi {
constexpr int CAPTION=1;
inline int fontFor(const GfxRenderer&,int preferred,const char*) {return preferred;}
struct Run {Rect r; int percent=-1;};
std::vector<Run> runs;
template<class R>
void text(GfxRenderer& g,R r,const char*,int,bool=false,freeink::ui::TextAlign=freeink::ui::TextAlign::Left) {
  assert(r.width>0 && r.height>0 && r.x>=5 && r.y>=5);
  assert(r.x+r.width<=g.w-5 && r.y+r.height<=g.h-8);
  runs.push_back({{r.x,r.y,r.width,r.height}});
}
// Production passes braced rectangles; use a concrete type at that boundary.
void text(GfxRenderer& g,Rect r,const char* s,int f,bool inverted=false,freeink::ui::TextAlign a=freeink::ui::TextAlign::Left) {
  text<Rect>(g,r,s,f,inverted,a);
}
void secondaryText(GfxRenderer& g,Rect r,const char* s,int f,bool inverted=false,freeink::ui::TextAlign a=freeink::ui::TextAlign::Left) {
  text<Rect>(g,r,s,f,inverted,a);
}
void progress(GfxRenderer& g,Rect r,int percent,bool=false,bool lineTrack=false) {
  assert(!lineTrack);  // the gray track stays off the Plus
  assert(r.x>=5 && r.y>=5 && r.x+r.width<=g.w-5 && r.y+r.height<=g.h-8);
  runs.push_back({r,std::clamp(percent,0,100)});
}
}
struct BaseTheme {
  static void drawRickyReaderChrome(GfxRenderer&,float,int,int,const char*,bool,bool,bool=true,const Rect* =nullptr);
  static void drawBluetoothStatusIcon(GfxRenderer&,int,int) {}
  static void drawBookmarkStatusIcon(GfxRenderer&,int,int) {}
  static void drawBatteryLeft(GfxRenderer&,Rect,bool) {}
} gui;
#define GUI gui
''' + method(source, 'void BaseTheme::drawRickyReaderChrome(') + r'''
int main() {
  for (const auto size : {Rect{0,0,684,1216},Rect{0,0,1216,684},Rect{0,0,600,1000},Rect{0,0,1000,600}})
  for (const int rowHeight : {28,34,42,56}) {
    GfxRenderer r{size.width,size.height,rowHeight};
    settings.spec.progressBarMode=0;
    for(int flags=0;flags<256;++flags) {
      settings.spec.lane=flags&1;
      settings.spec.showChapterPageCount=flags&2;
      settings.spec.showBookProgressPercent=flags&4;
      settings.spec.bar=flags&8;
      settings.spec.clockMode=(flags>>4)&1;
      settings.spec.showBattery=flags&64;
      settings.spec.clock=flags&128;
      bleinput::connected=flags&32;
      RickyDesignUi::runs.clear();
      BaseTheme::drawRickyReaderChrome(r,42.0f,3,11,"章节",true,true);
      const int expected=(settings.spec.lane ? 1+bool(flags&2)+bool(flags&4):0)+bool(flags&8);
      assert(static_cast<int>(RickyDesignUi::runs.size())==expected);
      if(flags&8) {
        const auto track=RickyDesignUi::runs.back();
        const auto chrome=readerChrome::compactLayout(rowHeight,settings.spec.headerLaneVisible(),
                                                      settings.spec.footerLaneVisible(),true,6);
        assert(track.percent==42);
        if(settings.spec.footerLaneVisible()) {
          // One footer row: the bar sits on it, between the page count and the percent.
          const int textY=r.h-8-chrome.footerTextBottom;
          assert(track.r.y>=textY && track.r.y+track.r.height<=textY+chrome.rowHeight);
          for(size_t i=0;i+1<RickyDesignUi::runs.size();++i) {
            const auto& run=RickyDesignUi::runs[i];
            if(run.r.y!=textY) continue;  // header runs
            assert(run.r.x>=track.r.x+track.r.width || run.r.x<track.r.x);
          }
        } else assert(track.r.y+track.r.height==r.h-8-4);
        assert(track.r.y>=r.h-8-chrome.footerReserve+8);
      }
    }
    // Footer only (the puppy reading mode): the header's title run is not drawn.
    settings.spec=Spec{};
    RickyDesignUi::runs.clear();
    BaseTheme::drawRickyReaderChrome(r,42.0f,3,11,"章节",false,false,false);
    assert(static_cast<int>(RickyDesignUi::runs.size())==3);
    const auto chrome=readerChrome::compactLayout(rowHeight,true,true,true,6);
    // A hidden footer (no page count, no percent, no bar) reserves nothing.
    assert(readerChrome::compactLayout(rowHeight,true,false,false,6).footerReserve==0);
    assert(readerChrome::compactLayout(rowHeight,false,true,false,6).headerReserve==0);
    for(const auto& run : RickyDesignUi::runs) assert(run.r.y>=r.h-8-chrome.footerReserve+8);
    settings.spec.lane=false;
    settings.spec.bar=true;
    settings.spec.progressBarMode=1;
    RickyDesignUi::runs.clear();
    BaseTheme::drawRickyReaderChrome(r,42,3,0,"",false,false);
    assert(RickyDesignUi::runs.back().percent==0);
    BaseTheme::drawRickyReaderChrome(r,140,15,10,"",false,false);
    assert(RickyDesignUi::runs.back().percent==100);
  }
}
''', include_dirs=(ROOT / 'src',))

    def test_reservation_and_render_share_tokens_without_forcing_reader_preferences(self):
        reader = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        # One margin source: renderBook and the pickers take textMargins(), which
        # reserves the header and footer bands the reading mode keeps.
        margins = method(reader, 'readingMode::Margins EpubReaderActivity::textMargins(')
        self.assertIn('readerChrome::compactLayout(', margins)
        self.assertIn('chrome.headerReserve', margins)
        self.assertIn('std::max(viewable.top + marginTop, topFloor)', margins)
        self.assertIn('bottomReserve = chrome.footerReserve', margins)
        self.assertNotIn('kReaderFooterHeight', margins)
        self.assertIn('sb.textLaneVisible()', margins)
        self.assertIn('readingMode::showsHeader(mode)', margins)
        self.assertIn('readingMode::showsFooter(mode)', margins)
        book = method(reader, 'void EpubReaderActivity::renderBook(')
        self.assertIn('textMargins()', book)
        status = method(reader, 'void EpubReaderActivity::renderStatusBar(')
        self.assertIn('GUI.drawRickyReaderChrome', status)
        self.assertIn('GUI.drawStatusBar', status)  # legacy/auto-turn compatibility
        self.assertNotIn('SETTINGS.', status[status.index('GUI.drawRickyReaderChrome'):status.index('return;')])
        self.assertIn('renderer.truncatedText(RickyDesignUi::BODY', status)
        self.assertIn('title != epub->getTitle()', status)
        # Word/footnote selection must use exactly the new body's origin,
        # not the old screen margin alone (otherwise highlight/touch drifts).
        for entry in ('void EpubReaderActivity::openDictionaryWordSelect(',
                      'void EpubReaderActivity::openFootnoteSelect('):
            selection = method(reader, entry)
            self.assertIn('textMargins()', selection)
            self.assertNotIn('getOrientedViewableTRBL', selection)

    def test_toolbar_keeps_action_ids_and_uses_one_icon_weight_and_selection_language(self):
        toolbar = (ROOT / 'src/activities/reader/ReaderToolbarUi.cpp').read_text()
        for icon in ('contents', 'fonts', 'more', 'back', 'next'):
            self.assertIn(f'icon_ricky_{icon}_40', toolbar)
        for statement in ('titleStyle.bold = !UiHighDpiProfile::enabled;',
                          'listProps_.valueText.bold = !UiHighDpiProfile::enabled;',
                          'ricky::ds::radius::kControl', 'ricky::ds::space::kTouchSpacing'):
            self.assertIn(statement, toolbar)
        self.assertIn('ACTION_TOOL, static_cast<int16_t>(kSegmentToolBase + i)', toolbar)
        self.assertIn('if (pending_.event == Event::Scrub && !touch.snap.touchReleased)', toolbar)

    def test_overlay_retains_snapshot_fast_path_and_does_not_redraw_page_for_chrome(self):
        reader = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        for name in ('openOverlay', 'closeOverlayToPage'):
            body = method(reader, f'void EpubReaderActivity::{name}(')
            self.assertIn('renderer.restoreBwBuffer', body)
            self.assertNotIn('beginGrayscale16', body)
        self.assertIn('renderer.displayBufferAsync(HalDisplay::FAST_REFRESH)',
                      method(reader, 'void EpubReaderActivity::pushOverlayRefresh('))

    def test_menu_uses_mockup_cards_and_visible_home_retains_result_contract(self):
        source = (ROOT / 'src/activities/reader/EpubReaderMenuActivity.cpp').read_text()
        build = method(source, 'void EpubReaderMenuActivity::buildScreen(')
        self.assertIn('RickyDesignUi::card(renderer, cell, selected)', build)
        self.assertIn('space::kGutter', build)
        self.assertIn('screen.frame().hit(home, ACTION_USER', build)
        enter = method(source, 'void EpubReaderMenuActivity::onEnter(')
        self.assertIn('UiListActivity::onEnter()', enter)
        self.assertIn('finishWithAction(MenuAction::GO_HOME)', enter)
        run_cpp(r'''
#include <array>
#include <cassert>
#include <cstdint>
struct App {int cleared=0; void clearTapFlash() {++cleared;}};
struct MenuResult {int action; uint8_t orientation,speed;};
struct EpubReaderMenuActivity {
  enum class MenuAction {GO_HOME=12};
  App app; int finishes=0; uint8_t pendingOrientation=3,selectedPageTurnOption=0;
  MenuResult result{};
  void setResult(MenuResult value) {result=value;}
  void finish() {++finishes;}
  void finishWithAction(MenuAction);
};
''' + method(source, 'void EpubReaderMenuActivity::finishWithAction(') + r'''
int main() {
 for(uint8_t i=0;i<5;++i) {
  EpubReaderMenuActivity a; a.selectedPageTurnOption=i;
  a.finishWithAction(EpubReaderMenuActivity::MenuAction::GO_HOME);
  assert(a.result.action==12 && a.result.orientation==3);
  assert((a.result.speed==std::array<int,5>{0,1,3,6,12}[i]));
  assert(a.finishes==1 && a.app.cleared==1);
 }
}
''')


if __name__ == '__main__':
    unittest.main()
