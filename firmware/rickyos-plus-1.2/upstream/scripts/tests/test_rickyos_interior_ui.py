"""Pushed-page design and production reader-slot mapping regressions."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyInteriorUiTest(unittest.TestCase):
    def test_all_reader_actions_remain_reachable_for_every_optional_combination(self):
        run_cpp(r'''
#include <cassert>
#include <array>
#include <vector>
#include "components/RickyReaderMenuLayout.h"
enum class Action {SELECT_CHAPTER, FOOTNOTES, TEXT_SETTINGS, NIGHT_MODE, FRONTLIGHT,
  GO_TO_PERCENT, AUTO_PAGE_TURN, ROTATE_SCREEN, BOOKMARKS, TOGGLE_BOOKMARK,
  SCREENSHOT, DISPLAY_QR, GO_HOME, SYNC, DELETE_CACHE, DICTIONARY, READING_MODE, READING_LINE,
  FONT_SIZE, TEXT_WEIGHT, HOLDING_HAND};
struct Item {Action action;};
int main() {
  for(int flags=0; flags<8; ++flags) {
    std::vector<Item> items;
    for(int action=0; action<21; ++action) {
      const auto a=static_cast<Action>(action);
      if(a==Action::FOOTNOTES && !(flags&1)) continue;
      if(a==Action::BOOKMARKS && !(flags&2)) continue;
      if(a==Action::FRONTLIGHT && !(flags&4)) continue;
      items.push_back({a});
    }
    const auto root=RickyReaderMenuLayout::build<Action>(items,false);
    const auto more=RickyReaderMenuLayout::build<Action>(items,true);
    assert(root.quickCount==4);
    assert(root.count<=12 && more.count<=10);
    // 惯用手 sits in More, right after 屏幕方向.
    for(int i=0; i+1<more.count; ++i)
      if(items[more.indices[i]].action==Action::ROTATE_SCREEN)
        assert(items[more.indices[i+1]].action==Action::HOLDING_HAND);
    // 字号 and 字重 are one tap from the page.
    bool size=false, weight=false;
    for(int i=root.quickCount; i<root.count; ++i) {
      if(root.indices[i]==RickyReaderMenuLayout::More) continue;
      size|=items[root.indices[i]].action==Action::FONT_SIZE;
      weight|=items[root.indices[i]].action==Action::TEXT_WEIGHT;
    }
    assert(size && weight);
    assert(items[root.indices[0]].action==Action::SELECT_CHAPTER);
    assert(items[root.indices[1]].action==Action::TOGGLE_BOOKMARK);
    assert(items[root.indices[2]].action==Action::TEXT_SETTINGS);
    assert(items[root.indices[3]].action==Action::DICTIONARY);
    assert(root.indices[root.count-1]==RickyReaderMenuLayout::More);
    std::array<int,RickyReaderMenuLayout::kMaxItems> seen{};
    for(const auto view : {root,more}) {
      for(int i=0; i<view.count; ++i) {
        const int index=view.indices[i];
        if(index==RickyReaderMenuLayout::More) continue;
        assert(index>=0 && index<static_cast<int>(items.size()));
        ++seen[index];
      }
    }
    for(size_t i=0; i<items.size(); ++i) assert(seen[i]==1);
  }
}
''', include_dirs=(ROOT / "src",))

    def test_popup_underlay_preserves_outer_capture_and_never_submits_intermediate_frame(self):
        source = (ROOT / "src/activities/UiListActivity.cpp").read_text()
        run_cpp(r'''
#include <cassert>
#include <initializer_list>
struct Renderer {
  bool capture=false;
  int submits=0, clears=0;
  bool isDisplayCapture() const {return capture;}
  void setDisplayCapture(bool value) {capture=value;}
  void clearScreen() {++clears;}
  void displayBuffer() {if(!capture) ++submits;}
};
struct UiListActivity {
  Renderer renderer;
  int chrome=0, ui=0, footer=0;
  void drawChrome() {++chrome;}
  void renderUi() {++ui; renderer.displayBuffer();}
  void drawFooter() {++footer;}
  void renderPopupUnderlay();
};
''' + method(source, 'void UiListActivity::renderPopupUnderlay(') + r'''
int main() {
  for(bool capture : {false,true}) {
    UiListActivity activity;
    activity.renderer.capture=capture;
    activity.renderPopupUnderlay();
    assert(activity.renderer.capture==capture);
    assert(activity.renderer.submits==0 && activity.renderer.clears==1);
    assert(activity.chrome==1 && activity.ui==1 && activity.footer==1);
  }
}
''')

    def test_storage_list_is_not_a_shelf_and_does_not_issue_render_time_storage_reads(self):
        source = (ROOT / "src/activities/home/FileBrowserActivity.cpp").read_text()
        self.assertIn('if (UiHighDpiProfile::enabled) return false;', method(source, 'bool FileBrowserActivity::usesIconLayout('))
        self.assertIn('if (!usesIconLayout())', method(source, 'void FileBrowserActivity::rebuildRowItems('))
        provider = method(source, 'void FileBrowserActivity::provideRow(')
        self.assertIn('STR_RICKY_FOLDER_TYPE', provider)
        self.assertIn('item.subtitle = extension', provider)
        self.assertNotIn('Storage.', provider)
        self.assertNotIn('openFile', provider)

    def test_pushed_header_and_dialog_use_real_faces_and_unified_geometry(self):
        base = (ROOT / "src/components/themes/BaseTheme.cpp").read_text()
        header = method(base, 'void BaseTheme::drawHeaderWithStyle(')
        product = header[:header.index('// Every activity header')]
        for fragment in ('kStatusBarHeight', 'kPageMargin', 'RickyDesignUi::HEADLINE', 'icon_ricky_back_40', 'HeaderBackTapTarget::setLeading'):
            self.assertIn(fragment, product)
        self.assertNotIn('EpdFontFamily::BOLD', product)
        popup = method((ROOT / 'src/components/themes/inx/InxTheme.cpp').read_text(), 'void InxTheme::drawOptionPopup(')
        self.assertIn('RickyDesignUi::HEADLINE', popup)
        self.assertIn('radius::kControl', popup)
        self.assertNotIn('EpdFontFamily::BOLD', popup)
        self.assertNotIn('drawDottedSeparator', popup)

    def test_bitmap_adapter_keeps_scaling_rotation_unknown_assets_and_clip_contracts(self):
        source = (ROOT / 'src/components/RickyUiTarget.h').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <FreeInkUICore.h>
namespace fui=freeink::ui;
struct GfxRenderer {
  bool native=true;
  mutable fui::Rect clip{10,20,80,80};
  mutable int twins=0,x=0,y=0;
  mutable bool ink=true;
  struct ClipScope {
    const GfxRenderer& r;
    fui::Rect old;
    ClipScope(const GfxRenderer& renderer,int x,int y,int w,int h):r(renderer),old(renderer.clip) {
      r.clip={static_cast<int16_t>(x),static_cast<int16_t>(y),static_cast<int16_t>(w),static_cast<int16_t>(h)};
    }
    ~ClipScope(){r.clip=old;}
  };
};
namespace RickyAaIcons {
  bool draw(const GfxRenderer& r,const uint8_t* bits,int,int,int x,int y,bool ink) {
    if(!r.native || *bits!=42) return false;
    ++r.twins; r.x=x; r.y=y; r.ink=ink;
    assert(r.clip.x>=10 && r.clip.y>=20 && r.clip.right()<=90 && r.clip.bottom()<=100);
    return true;
  }
}
struct BitmapBase {
  virtual void bitmap(fui::Rect,fui::BitmapRef,fui::BitmapMode,fui::Paint,fui::Rotation)=0;
};
struct RickyUiTarget:BitmapBase {
  GfxRenderer renderer_;
  struct Target {
    int fallbacks=0;
    void bitmap(fui::Rect,fui::BitmapRef,fui::BitmapMode,fui::Paint,fui::Rotation){++fallbacks;}
  } target_;
  fui::Rect clipRect() const {return renderer_.clip;}
''' + method(source, 'void bitmap(') + r'''
};
int main() {
  RickyUiTarget target;
  const uint8_t known=42, unknown=0;
  const fui::BitmapRef icon{&known,40,40,fui::BitmapFormat::Mask1};
  target.bitmap({0,0,80,80},icon,fui::BitmapMode::Center);
  assert(target.renderer_.twins==1 && target.renderer_.x==20 && target.renderer_.y==20);
  assert(target.renderer_.clip.x==10 && target.renderer_.clip.y==20);
  target.bitmap({10,20,40,40},icon,fui::BitmapMode::Contain,fui::Paint::solid(fui::Color::White));
  assert(target.renderer_.twins==2 && !target.renderer_.ink);
  target.bitmap({10,20,32,32},icon,fui::BitmapMode::Contain);
  target.bitmap({10,20,40,40},icon,fui::BitmapMode::Center,fui::Paint::solid(fui::Color::Black),fui::Rotation::CW90);
  target.bitmap({10,20,40,40},{&unknown,40,40,fui::BitmapFormat::Mask1},fui::BitmapMode::Center);
  target.bitmap({10,20,40,40},{&known,40,40,fui::BitmapFormat::BW1},fui::BitmapMode::Center);
  target.renderer_.native=false;
  target.bitmap({10,20,40,40},icon,fui::BitmapMode::Center);
  assert(target.target_.fallbacks==5 && target.renderer_.twins==2);
}
''', include_dirs=(ROOT / 'freeink-sdk/libs/ui/FreeInkUI/include',))

    def test_runtime_metrics_do_not_reapply_old_profile_over_product_tokens(self):
        source = (ROOT / 'src/components/UITheme.cpp').read_text()
        self.assertNotIn('UiHighDpiProfile::apply(', method(source, 'const ThemeMetrics& UITheme::getMetrics('))


if __name__ == '__main__':
    unittest.main()
