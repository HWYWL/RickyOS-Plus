"""Host checks for the production RickyOS layout, using the actual geometry helper."""
from pathlib import Path
import json
import re
import subprocess
import tempfile
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyHomeTest(unittest.TestCase):
    def test_actual_sdk_character_wrap_preserves_chinese_and_long_nickname(self):
        source = (ROOT / 'src/components/RickyPageUi.h').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <string>
#include <vector>
#include "FreeInkUICore.h"
namespace fui = freeink::ui;
struct Target : fui::DrawTarget {
  std::vector<fui::Rect> blocks;
  std::vector<std::string> runs;
  fui::Size measureText(fui::FontId,const char* text,fui::TextStyle) const override {
    int width=0;
    for(auto p=reinterpret_cast<const unsigned char*>(text);*p;++p)
      if((*p&0xC0)!=0x80) width+=*p<128?8:20;
    return {static_cast<int16_t>(width),34};
  }
  int16_t lineHeight(fui::FontId) const override {return 34;}
  void fill(fui::Rect,fui::Paint,uint8_t,uint8_t) override {}
  void stroke(fui::Rect,fui::Paint,uint8_t,uint8_t,uint8_t) override {}
  void line(fui::Point,fui::Point,uint8_t,fui::Paint) override {}
  void triangle(fui::Point,fui::Point,fui::Point,fui::Paint) override {}
  void bitmap(fui::Rect,fui::BitmapRef,fui::BitmapMode,fui::Paint,fui::Rotation) override {}
  void text(fui::Rect rect,const char* text,fui::TextStyle style) override {
    assert(style.maxLines==1 && style.align==fui::TextAlign::Left);
    assert((static_cast<unsigned char>(*text)&0xC0)!=0x80);
    blocks.push_back(rect);runs.emplace_back(text);
  }
};
''' + method(source, 'inline void wrappedText(') + r'''
int main() {
  for(auto title: {"为什么伟大不能被计划", "注意力商人：他们如何操控人心"}) {
    Target target;
    fui::TextStyle style;style.maxLines=2;
    fui::Rect rect{10,20,160,68};
    wrappedText(target,rect,title,style);
    assert(target.runs.size()==2);
    assert(target.runs[0]+target.runs[1]==title);
    for(auto r:target.blocks) {
      assert(r.x>=rect.x && r.y>=rect.y);
      assert(r.right()<=rect.right() && r.bottom()<=rect.bottom());
    }
  }
  {
    Target target;
    fui::TextStyle style;style.maxLines=2;
    const char* greeting="Rickyaaaaaaaaaaaaaaaaaaaa，愿今天有个好故事。";
    wrappedText(target,{0,0,260,68},greeting,style);
    assert(target.runs.size()==2 && target.runs[0]+target.runs[1]==greeting);
  }
  {
    Target target;
    fui::TextStyle style;style.maxLines=2;style.align=fui::TextAlign::Center;
    const char* title="很长很长的书名很长很长的书名很长很长的书名";
    wrappedText(target,{10,20,100,68},title,style);
    assert(target.runs.size()==2 && target.runs.back().ends_with("…"));
    auto measured=fui::measureWrappedText(target,title,style,100);
    assert(measured.height==static_cast<int>(target.runs.size())*34);
  }
}
''', include_dirs=(ROOT / 'freeink-sdk/libs/ui/FreeInkUI/include',))
        home = method((ROOT / 'src/components/RickyHomeUi.cpp').read_text(), 'void RickyHomeUi::drawPortrait(')
        self.assertIn('measureWrappedText(target, greetingText, greetingStyle', home)
        self.assertIn('design::BODY, false, fui::TextAlign::Left, 2)', re.sub(r'\s+', ' ', home))
        self.assertIn('titleOf(item), design::BODY, selectedBook, fui::TextAlign::Center', re.sub(r'\s+', ' ', home))

    def test_portrait_follows_the_quiet_home_layout(self):
        source = method((ROOT / 'src/components/RickyHomeUi.cpp').read_text(),
                        'void RickyHomeUi::drawPortrait(')
        # 1.2: continue card, two actual statistic cards, then four recent covers.
        order = [source.index(key) for key in ('STR_CONTINUE_READING', 'STR_RICKY_TODAY_READING',
                                               'STR_RICKY_HOME_RECENT', 'paint(cover, index)')]
        self.assertEqual(order, sorted(order))
        self.assertIn('screen.frame().hit(cell, OPEN_BOOK, index', source)
        self.assertIn('screen.frame().hit(cell, OPEN_BOOK, STATISTICS', source)
        # Two title lines and a metadata lane must fit even with BODY fallback.
        self.assertIn('shelf.height - 120', source)
        self.assertIn('gap * 3) / 4', source)
        self.assertIn('thumbnailHeight = coverH', source)
        self.assertIn('showSelection && selected == index', source)
        self.assertNotIn('SyntheticBoldScope', source)

    def test_real_cover_renderer_preserves_ratio_and_centers_without_changing_stock(self):
        source = (ROOT / 'src/activities/home/InxRecentActivity.cpp').read_text()
        program = r'''
#include <cassert>
#include <initializer_list>
#include "components/RickyHomeLayout.h"
struct Rect { int x,y,width,height; };
struct Bitmap { int w,h; bool gray=false; int getWidth() const {return w;} int getHeight() const {return h;}
  bool is1Bit() const {return !gray;} };
struct GfxRenderer {
  mutable Rect drawn{};
  mutable bool cropped = false, grayPath = false;
  bool active = false;
  bool isGrayscale16Active() const {return active;}
  bool drawBitmap(const Bitmap&, int x,int y,int w,int h) const {drawn={x,y,w,h};return true;}
  bool drawBitmapGrayscale16(const Bitmap&, int x,int y,int w,int h) const {grayPath=true;drawn={x,y,w,h};return true;}
  bool drawBitmapCropToFill(const Bitmap&, int x,int y,int w,int h) const {
    cropped=true;drawn={x,y,w,h};return true;
  }
};
''' + method(source, 'bool drawRecentCover(') + r'''
int main() {
  Rect cell{30,210,200,300};
  for (Bitmap b : {Bitmap{600,900},Bitmap{900,600},Bitmap{500,1000},Bitmap{800,800},Bitmap{80,120}}) {
    GfxRenderer r;
    assert(drawRecentCover(r,b,cell));
#ifdef RICKYOS_PRODUCT
    assert(!r.cropped);
    assert(r.drawn.width>0 && r.drawn.height>0);
    assert(r.drawn.width<=cell.width && r.drawn.height<=cell.height);
    assert(r.drawn.width<=b.w && r.drawn.height<=b.h);
    assert(r.drawn.x==cell.x+(cell.width-r.drawn.width)/2);
    assert(r.drawn.y==cell.y+(cell.height-r.drawn.height)/2);
    assert(std::abs(r.drawn.width*b.h-r.drawn.height*b.w) < std::max(b.w,b.h));
    // Gray thumbnails on a 16-level frame take the gray path into the same box; 1-bit never does.
    GfxRenderer g; g.active=true;
    Bitmap gray=b; gray.gray=true;
    assert(drawRecentCover(g,gray,cell) && g.grayPath);
    assert(g.drawn.x==r.drawn.x && g.drawn.y==r.drawn.y && g.drawn.width==r.drawn.width && g.drawn.height==r.drawn.height);
    GfxRenderer mono; mono.active=true;
    assert(drawRecentCover(mono,b,cell) && !mono.grayPath);
#else
    assert(r.cropped && r.drawn.width==cell.width && r.drawn.height==cell.height);
#endif
  }
  assert(RickyHomeLayout::fitBitmap(0,100,200,300).width==0);
  assert(RickyHomeLayout::fitBitmap(100,100,0,300).width==0);
}
'''
        for defines in ((), ('RICKYOS_PRODUCT',)):
            run_cpp(program, include_dirs=(ROOT / 'src',), defines=defines)
        actual = source[source.index('bool InxRecentActivity::tryDrawBookCover('):
                        source.index('bool InxRecentActivity::drawBookCover(')]
        self.assertEqual(actual.count('drawRecentCover(renderer, bitmap, bounds)'), 2)
        self.assertIn('activity.setThumbnailHeight(activity.rickyHome->coverHeight())', source)
        self.assertNotIn('activity.setThumbnailHeight(bounds.height)', source)
        home = method((ROOT / 'src/components/RickyHomeUi.cpp').read_text(), 'void RickyHomeUi::drawPortrait(')
        # Recent tiles are typographic; only the continue cover uses the cache size.
        self.assertIn('thumbnailHeight = coverH;', home)

    def test_greeting_varies_per_visit_not_per_render_and_fits_existing_buffer(self):
        run_cpp(r'''
#include <cassert>
#include "components/RickyHomeLayout.h"
int main() {
  unsigned seen=0;
  uint8_t previous=RickyHomeLayout::GREETING_COUNT;
  for(uint32_t seed=0;seed<10000;++seed) {
    auto next=RickyHomeLayout::chooseGreeting(seed,previous);
    assert(next<RickyHomeLayout::GREETING_COUNT && next!=previous);
    seen|=1U<<next;
    previous=next;
  }
  assert(seen==(1U<<RickyHomeLayout::GREETING_COUNT)-1);
}
''', include_dirs=(ROOT / 'src',))
        source = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        self.assertIn('chooseGreeting', method(source, 'void RickyHomeUi::begin('))
        self.assertNotIn('chooseGreeting', method(source, 'void RickyHomeUi::draw('))
        portrait = method(source, 'void RickyHomeUi::drawPortrait(')
        self.assertNotIn('chooseGreeting', portrait)
        self.assertNotIn('STR_RICKY_HOME_HELLO', portrait)
        self.assertIn('RickyProfile::nickname()', portrait)
        self.assertIn('greetingStyle.maxLines = 2', portrait)
        for language in ('chinese', 'english'):
            translations = (ROOT / f'lib/I18n/translations/{language}.yaml').read_text()
            formats = re.findall(r'^STR_RICKY_HOME_GREETING(?:_\w+)?: "(.*)"$', translations, re.M)
            self.assertEqual(len(formats), 5)
            literals = ','.join(json.dumps(item, ensure_ascii=False) for item in formats)
            run_cpp(r'''
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
  const char* formats[]={''' + literals + r'''};
  const char* nicknames[]={"Ricky","阅读生活阅读生活阅读生活阅读生活",
                         "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv"};
  for(auto format: formats) for(auto nickname: nicknames) {
    char greeting[128];
    const int size=std::snprintf(greeting,sizeof(greeting),format,nickname);
    assert(size>0 && size<static_cast<int>(sizeof(greeting)));
    assert(std::strstr(greeting,nickname)==greeting);
  }
}
''')

    def test_portrait_landscape_and_pagination(self):
        program = r'''
#include <cassert>
#include <initializer_list>
#include "components/RickyHomeLayout.h"
int main() {
  for (auto size : {std::pair{610, 1010}, std::pair{1126, 472},
                    std::pair{526, 796}, std::pair{916, 386}}) {
    for (int heading : {34, 40, 46}) for (int label : {28, 34, 40}) {
      auto m = RickyHomeLayout::fit(size.first, size.second, heading, label, 12, 12);
      assert(m.columns * m.rows == RickyHomeLayout::PAGE_BOOKS - 1);
      assert(m.coverHeight > 0);
      assert(m.coverHeight + label + 24 <= m.rowHeight);
      assert(m.coverHeight * 2 / 3 + 24 <= (size.first - (m.columns - 1) * 12) / m.columns);
      assert((m.rows + 1) * m.rowHeight + 2 * heading + (m.rows + 2) * 12 <= size.second);
      assert(m.rowHeight >= 44);
      auto text = RickyHomeLayout::fitText(m.coverHeight, heading, label, 12, m.rows == 1);
      const int progressRow = text.progress ? label : 6;
      const int textHeight = text.titleLines * heading + (text.author ? 12 + label : 0);
      assert(textHeight + progressRow + 8 <= m.coverHeight);
    }
  }
  for (int selection = 0; selection < 20; ++selection) {
    auto start = RickyHomeLayout::pageStart(selection);
    assert(start <= selection && selection < start + RickyHomeLayout::PAGE_BOOKS);
  }
  assert(RickyHomeLayout::pageStart(-1) == 0);
}
'''
        with tempfile.TemporaryDirectory(prefix="ricky-home-") as directory:
            source = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            source.write_text(program)
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_touch_is_routed_through_host_not_legacy_bridge(self):
        source = (ROOT / "src/components/RickyHomeUi.cpp").read_text()
        self.assertIn("routeTouch(input)", source)
        self.assertIn("touch.snap.touchReleased", source)
        self.assertNotIn("wasTapInRect", source)
        self.assertNotIn("wasScreenTapped", source)
        self.assertIn("grid.minTouchSize = 0", source)


if __name__ == "__main__":
    unittest.main()
