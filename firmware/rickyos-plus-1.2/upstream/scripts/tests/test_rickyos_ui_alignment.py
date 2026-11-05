"""Production layout, phrase persistence and advertised shelf filter regressions."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyUiAlignmentTest(unittest.TestCase):
    def test_brand_navigation_labels_icons_and_separator_fit(self):
        source = (ROOT / 'src/components/themes/inx/InxTheme.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
#include "activities/MainTab.h"
#include "components/RickyPageLayout.h"
constexpr int SMALL_FONT_ID=0;
constexpr int kIconSize=40, kPillWidth=88, kPillHeight=48;  // design tokens, both density profiles
namespace EpdFontFamily {enum Style {REGULAR,BOLD};}
enum class Color {DarkGray};
enum class StrId {STR_RICKY_NAV_HOME,STR_LIBRARY,STR_RICKY_STORAGE,STR_APPS_TITLE,STR_SETTINGS_TITLE};
struct {const char* get(StrId id) const {
 static constexpr const char* labels[]={"Home","Library","Storage","Apps","Settings"};
 return labels[static_cast<int>(id)];
}} I18N;
struct CrossPointSettings {static constexpr int INX_TAB_BOTTOM=1;};
struct {int inxTabPosition=1;} SETTINGS;
struct GfxRenderer {
 mutable std::vector<Rect> blocks;
 mutable int separators=0,labels=0,icons=0,selectedIcons=0;
 int lineHeight=UiHighDpiProfile::enabled?34:20;
 struct ClipScope {ClipScope(const GfxRenderer&,int,int,int,int){}};
 int getLineHeight(int) const {return lineHeight;}
 int getTextWidth(int,const char* s) const {return std::strlen(s)*6;}
 void fillRect(int x,int y,int w,int h,bool ink) const {if(ink){++separators;blocks.emplace_back(x,y,w,h);}}
 void drawText(int f,int x,int y,const char* s,bool) const {
  ++labels;blocks.emplace_back(x,y,getTextWidth(f,s),lineHeight);
 }
 bool gray=false;
 bool isGrayscale16Active() const {return gray;}
 void fillGrayscale16Rect(int x,int y,int w,int h,uint8_t) const {++separators;blocks.emplace_back(x,y,w,h);}
 struct TextInkScope {TextInkScope(const GfxRenderer&,uint8_t){}};
};
const uint8_t* iconForTab(MainTab) {static uint8_t ink=0;return &ink;}
void drawInxIcon(const GfxRenderer& r,const uint8_t*,int x,int y) {++r.icons;r.blocks.emplace_back(x,y,kIconSize,kIconSize);}
void drawSelectedInxIcon(const GfxRenderer& r,const uint8_t*,int x,int y,int maxWidth) {
 const int w=std::max(kIconSize+8,std::min(kPillWidth,maxWidth-8));
 ++r.selectedIcons;r.blocks.emplace_back(x+(kIconSize-w)/2,y+(kIconSize-kPillHeight)/2,w,kPillHeight);
}
struct InxTheme {void drawMainTabBar(const GfxRenderer&,Rect,MainTab) const;};
''' + method(source, 'void InxTheme::drawMainTabBar(') + r'''
int main() {
 for (int width : {360,480,684,1216}) for (int placement : {0,1})
 for (MainTab selected : MainTabs::values) {
  GfxRenderer renderer; SETTINGS.inxTabPosition=placement;
  Rect bar{5,17,width,MainTabs::bottomBarHeight};
  InxTheme{}.drawMainTabBar(renderer,bar,selected);
  // The rule and the current tab's mark; every icon plain (no capsule), every label.
  assert(renderer.separators==2 && renderer.icons==5 && renderer.labels==5);
  assert(renderer.selectedIcons==0);
  for(auto b:renderer.blocks) {
   assert(b.x>=bar.x && b.y>=bar.y);
   assert(b.x+b.width<=bar.x+bar.width && b.y+b.height<=bar.y+bar.height);
  }
  // The capsule, not just the icon, keeps clear of the label row.
  const auto rows=RickyPageLayout::navigationRows(bar,kPillHeight,renderer.lineHeight);
  assert(rows.labelY-rows.iconY-kPillHeight>=6);
 }
}
''', include_dirs=(ROOT / 'src',), defines=('RICKYOS_PRODUCT', 'CROSSMUX_UI_PROFILE_HIGH_DPI'))

    def test_1_2_chrome_uses_inverse_chips_and_native_gray_progress(self):
        source = (ROOT / 'src/activities/library/LibraryListActivity.cpp').read_text()
        shelf = method(source, 'void LibraryListActivity::buildRickyShelf(')
        for key in ('STR_RICKY_LIBRARY_COUNT', 'kTabPillHeight', 'picked ? fui::Color::White'):
            self.assertIn(key, shelf)
        self.assertIn('screen.frame().hit(rect, ACTION_READING_FILTER', shelf)
        self.assertNotIn('button.state = readingFilter', shelf)
        home = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        portrait = method(home, 'void RickyHomeUi::drawPortrait(')
        for key in ('STR_RICKY_HOME_MONTH_DAY','STR_RICKY_TODAY_READING','STR_RICKY_STREAK_READING',
                    'STR_RICKY_HOME_RECENT'):
            self.assertIn(key, portrait)
        self.assertIn('READING_STATS.findMatchingBookForPath', portrait)
        self.assertIn('RickyProfile::homePhrase()', portrait)
        self.assertIn('design::progress(', portrait)
        helper = (ROOT / 'src/components/RickyDesignUi.h').read_text()
        self.assertNotIn('Paint::dither', helper)
        self.assertIn('ds::gray::kTint', method(helper, 'inline void progress('))

    def test_main_pages_omit_decorative_explanations_and_their_reservations(self):
        # The promotional images carry explanatory captions; the product keeps them out.
        home = method((ROOT / 'src/components/RickyHomeUi.cpp').read_text(),
                      'void RickyHomeUi::drawPortrait(')
        self.assertNotIn('STR_RICKY_PHRASE_CAPTION', home)
        self.assertIn('screen.takeBottom(28, ds::space::k4)', home)
        self.assertIn('RickyProfile::homePhrase()', home)
        shelf = method((ROOT / 'src/activities/library/LibraryListActivity.cpp').read_text(),
                       'void LibraryListActivity::buildRickyShelf(')
        self.assertNotIn('STR_RICKY_LIBRARY_HINT', shelf)
        self.assertIn('multiplePages ? screen.takeBottom(labelHeight, gap)', shelf)
        self.assertNotIn('STR_SEARCH', shelf)  # No Chinese text entry yet: search stays hidden.
        storage = method((ROOT / 'src/activities/home/RickyStorageActivity.cpp').read_text(),
                         'void RickyStorageActivity::buildScreen(')
        for key in ('FIND', 'ORGANIZED', 'BOOK_DETAIL', 'FONT_DETAIL', 'IMAGE_DETAIL', 'DOWNLOAD_DETAIL'):
            self.assertNotIn('STR_RICKY_STORAGE_' + key, storage)
        self.assertNotIn('auto intro = screen.takeTop', storage)
        self.assertIn('STR_RICKY_FOLDER_MISSING', storage)
        # The SD card summary opens the whole card; file transfer lives in Apps and the
        # control center, not on the Storage page.
        self.assertIn('screen.frame().hit(summary, ACTION_ROW, 4', storage)
        self.assertNotIn('STR_RICKY_UPLOAD_FILES', storage)
        profile = method((ROOT / 'src/activities/settings/RickyProfileActivity.cpp').read_text(),
                         'void RickyProfileActivity::buildScreen(')
        self.assertNotIn('HINT', profile)
        self.assertIn('if (failed) {', profile)

    def test_generated_cover_drawing_stays_inside_frame(self):
        source = (ROOT / 'src/components/RickyPageUi.h').read_text()
        covers = source[source.index('enum class Motif'):source.index('// Landscape "recently opened" tile')]
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
struct Rect {int x,y,width,height; explicit Rect(int x=0,int y=0,int w=0,int h=0):x(x),y(y),width(w),height(h) {}};
namespace fui {
struct Rect {int16_t x,y,width,height; bool empty() const {return width<=0 || height<=0;}
             int16_t right() const {return x+width;} int16_t bottom() const {return y+height;}};
struct Point {int16_t x,y;};
struct Size {int16_t width,height;};
enum class Color {Black,White,LightGray,DarkGray};
struct Paint {static Paint solid(Color){return {};} static Paint dither(Color){return {};}};
enum class TextAlign {Left,Center,Right};
struct TextStyle {int font=0;TextAlign align=TextAlign::Left;int maxLines=0;bool bold=false;};
struct DrawTarget {
  std::vector<Rect> blocks;
  int16_t lineHeight(int) const {return 34;}
  Size measureText(int,const char* text,TextStyle) const {return {int16_t(strlen(text)*9),34};}
  void fill(Rect r,Paint,uint8_t=0) {blocks.push_back(r);}
  void stroke(Rect r,Paint,uint8_t,uint8_t=0) {blocks.push_back(r);}
  void line(Point a,Point b,uint8_t,Paint) {blocks.push_back({std::min(a.x,b.x),std::min(a.y,b.y),
      int16_t(std::abs(a.x-b.x)+1),int16_t(std::abs(a.y-b.y)+1)});}
  void text(Rect r,const char*,TextStyle s) {assert(s.maxLines>=1 && s.maxLines<=3);blocks.push_back(r);}
};
inline Size measureWrappedText(DrawTarget&,const char* text,TextStyle s,int width) {
  const int lines=std::min<int>(std::max(1,s.maxLines),int(strlen(text)*9/std::max(1,width))+1);
  return {int16_t(width),int16_t(lines*34)};
}
}
inline fui::Rect uiRect(const Rect& r) {return {int16_t(r.x),int16_t(r.y),int16_t(r.width),int16_t(r.height)};}
void wrappedText(fui::DrawTarget& t,fui::Rect r,const char* text,fui::TextStyle style) {t.text(r,text,style);}
''' + covers + r'''
int main() {
  assert(strcmp(coverAuthor("亨利·戴维·梭罗"),"梭罗")==0);
  assert(strcmp(coverAuthor("Henry David Thoreau"),"Henry David Thoreau")==0);
  std::string title="浮生六记.txt"; stripBookExtension(title); assert(title=="浮生六记");
  title="Walden.EPUB"; stripBookExtension(title); assert(title=="Walden");
  for (auto r : {fui::Rect{10,20,8,12},fui::Rect{10,20,60,90},fui::Rect{10,20,140,210},
                 fui::Rect{10,20,166,222},fui::Rect{10,20,234,353},fui::Rect{10,20,120,180}}) {
    for (const char* footer : {static_cast<const char*>(nullptr),"EPUB"}) {
      fui::DrawTarget target;
      generatedCover(target,r,"long title without an embedded cover","亨利·戴维·梭罗",footer,{});
      for (auto b : target.blocks) {
        assert(b.width>0 && b.height>0 && b.x>=r.x && b.y>=r.y);
        assert(b.x+b.width<=r.x+r.width && b.y+b.height<=r.y+r.height);
      }
    }
  }
}
''')

    def test_cards_keep_hit_geometry_inside_safe_content(self):
        source = (ROOT / 'src/components/RickyPageLayout.h').read_text()
        layout = source[source.index('namespace RickyPageLayout'):]
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <initializer_list>
struct Rect {
  int x,y,width,height;
  explicit Rect(int x=0,int y=0,int width=0,int height=0):x(x),y(y),width(width),height(height) {}
};
''' + layout + r'''
int main() {
  for (auto body : {Rect{38,110,608,840},Rect{38,200,608,604},
                    Rect{32,100,1148,440},Rect{20,100,480,550}}) {
    for (int count : {4,6}) for (int gap : {6,12,24}) {
      for (int i=0; i<count; ++i) {
        auto r=RickyPageLayout::cell(body,i,count,gap);
        assert(r.width>=44 && r.height>=44);
        assert(r.x>=body.x && r.y>=body.y);
        assert(r.x+r.width<=body.x+body.width && r.y+r.height<=body.y+body.height);
        for (int j=0;j<i;++j) {
          auto other=RickyPageLayout::cell(body,j,count,gap);
          assert(r.x>=other.x+other.width+gap || other.x>=r.x+r.width+gap ||
                 r.y>=other.y+other.height+gap || other.y>=r.y+r.height+gap);
        }
      }
    }
  }
  assert(RickyPageLayout::cell(Rect{},0,0,12).width==0);
  assert(RickyPageLayout::cell(Rect{},-1,4,12).width==0);
  // A portrait page's remaining body can be wider than tall after header/footer.
  const Rect shortBody{29,260,626,500};
  const auto first=RickyPageLayout::cell(shortBody,0,4,24,2);
  const auto third=RickyPageLayout::cell(shortBody,2,4,24,2);
  assert(first.x==third.x && third.y>=first.y+first.height+24);
}
''')

    def test_phrase_default_limits_cancel_and_failed_save(self):
        source = (ROOT / 'src/components/RickyProfile.cpp').read_text()
        run_cpp(r'''
#include <cassert>
#include <cstring>
#include <string>
struct Settings {
  char rickyHomePhrase[97]=""; bool fail=false; int saves=0;
  bool saveToFile() {++saves; return !fail;}
} SETTINGS;
constexpr int STR_RICKY_HOME_PHRASE=0;
const char* tr(int) {return "default";}
''' + method(source, 'const char* homePhrase(') + method(source, 'bool setHomePhrase(') + r'''
int main() {
  assert(std::string(homePhrase())=="default");
  assert(setHomePhrase("今天也读几页。"));
  assert(std::string(homePhrase())=="今天也读几页。");
  int saves=SETTINGS.saves;
  assert(setHomePhrase("今天也读几页。") && saves==SETTINGS.saves);
  SETTINGS.fail=true;
  assert(!setHomePhrase("change") && std::string(homePhrase())=="今天也读几页。");
  assert(!setHomePhrase("") && std::string(homePhrase())=="今天也读几页。");
  SETTINGS.fail=false;
  saves=SETTINGS.saves;
  assert(!setHomePhrase(std::string(97,'a')));
  assert(!setHomePhrase("line\nline") && !setHomePhrase(std::string("a\0b",3)));
  assert(!setHomePhrase("line\tline") && SETTINGS.saves==saves);
  assert(setHomePhrase(std::string(96,'a')));
  assert(std::strlen(homePhrase())==96);
  assert(setHomePhrase("") && std::string(homePhrase())=="default");
}
''')
        activity = (ROOT / 'src/activities/settings/RickyHomePhraseActivity.cpp').read_text()
        self.assertIn('if (result.isCancelled) return', activity)
        self.assertIn('waitForConfirmRelease = mappedInput.isPressed', activity)
        settings = (ROOT / 'src/CrossPointSettings.cpp').read_text()
        self.assertIn('doc["rickyHomePhrase"] = rickyHomePhrase', settings)
        self.assertIn('doc["rickyHomePhrase"] | ""', settings)
        self.assertIn('utf8SafeTruncateBuffer(rickyHomePhrase', settings)

    def test_1_2_filters_and_six_book_portrait_shelf_remain(self):
        source = (ROOT / 'src/activities/library/LibraryListActivity.cpp').read_text()
        shelf = method(source, 'void LibraryListActivity::buildRickyShelf(')
        for key in ('STR_RICKY_BOOKS_ALL','STR_RICKY_BOOKS_READING','STR_RICKY_BOOKS_UNREAD'):
            self.assertIn(key, shelf)
        self.assertIn('const int columns = body.width > body.height ? 6 : 3', shelf)
        header = (ROOT / 'src/activities/library/LibraryListActivity.h').read_text()
        self.assertIn('SHELF_CAPACITY = 6', header)
        self.assertNotIn('buildTabBar(screen)', shelf)
        self.assertIn('ACTION_SHELF_OPTIONS', shelf)
        self.assertIn('STR_RICKY_BOOK_PROGRESS', shelf)
        self.assertIn('stats->lastProgressPercent', shelf)
        filtering = method(source, 'void LibraryListActivity::applyFilter(')
        self.assertIn('readingFilter == 1 && !reading', filtering)
        self.assertIn('readingFilter == 2 && started', filtering)
        self.assertIn('started && !stats->completed', filtering)
        enter = method(source, 'void LibraryListActivity::onEnter(')
        self.assertIn('ACTION_SHELF_OPTIONS', enter)
        self.assertIn('self.optionPopup.show', enter)

    def test_shelf_title_and_progress_reserve_real_font_lines(self):
        shelf = method((ROOT / 'src/activities/library/LibraryListActivity.cpp').read_text(),
                       'void LibraryListActivity::buildRickyShelf(')
        self.assertIn('renderer.getLineHeight(RickyDesignUi::CAPTION)', shelf)
        self.assertIn('renderer.getLineHeight(RickyDesignUi::BODY)', shelf)
        self.assertIn('rowHeight - cardExtraHeight', shelf)
        self.assertIn('titleRect.bottom() + cardTextGap', shelf)
        self.assertNotIn('rect.bottom() + 36', shelf)

    def test_storage_does_not_create_or_reorganize_user_directories(self):
        source = (ROOT / 'src/activities/home/RickyStorageActivity.cpp').read_text()
        for key in ('STR_RICKY_DOWNLOADS','STR_RICKY_IMAGES','STR_FONT', 'STR_RICKY_BOOK_FILES',
                    'STR_RICKY_SD_CARD'):
            self.assertIn(key, source)
        # The page only creates the four fixed folders; it never renames or deletes.
        for mutation in ('Storage.rename', 'Storage.remove', 'Storage.rmdir'):
            self.assertNotIn(mutation, source)
        for folder in ('BOOKS', 'FONTS', 'IMAGES', 'DOWNLOADS'):
            self.assertIn('RickyStorageLayout::' + folder, source)
        self.assertIn('!Storage.exists(path)', source)
        self.assertIn('folderMissing = true', source)
        self.assertNotIn('RickyPageUi::tile(screen', source)
        # 1.2 folder cards use inverse selection, not a thicker outline.
        self.assertIn('RickyPageUi::card(target, rect, picked)', source)
        self.assertIn('RickyPageUi::pageIcon(', source)

    def test_font_tile_releases_parent_lists_and_system_keeps_controls(self):
        source = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        reorg = method(source, 'void SettingsActivity::reorganizeRickySettings(')
        self.assertIn('moveMatching(controlsSettings, systemSettings', reorg)
        self.assertIn('moveMatching(readerSettings, fontSettings', reorg)
        activate = method(source, 'void SettingsActivity::activateIndex(')
        font = activate[activate.index('if (index == 3)'):activate.index('openRickyCategory(index)')]
        self.assertLess(font.index('releaseListsForMemoryHungryChild()'), font.index('startActivityForResultWith'))
        self.assertIn('FontLibraryActivity', font)
        release = method(source, 'void SettingsActivity::releaseListsForMemoryHungryChild(')
        self.assertIn('swap(fontSettings)', release)


if __name__ == '__main__':
    unittest.main()
