"""Exercise the actual product icon routing/rasterizer without changing tab input."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from test_reading_ui_regressions import ROOT, method, run_cpp

HEADER = ROOT / 'src/components/icons/rickyNavigationIcons.h'
SOURCE = ROOT / 'src/components/themes/inx/InxTheme.cpp'
NAMES = ('home', 'library', 'apps', 'settings', 'statistics', 'storage')
NODE = os.environ.get('RICKY_ICON_NODE') or shutil.which('node')


def sharp_available():
    return NODE and subprocess.run([NODE, '-e', 'require("sharp")'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


class RickyNavigationIconTest(unittest.TestCase):
    def test_original_sources_and_native_white_padding(self):
        sources = ROOT / 'src/components/icons/sources/rickyos'
        for name in NAMES:
            svg = ET.parse(sources / f'{name}.svg').getroot()
            self.assertEqual(svg.attrib['viewBox'], '0 0 24 24')
            self.assertEqual(svg.attrib['stroke-width'], '1.2')  # 2 physical pixels at 40 px
            self.assertEqual(svg.attrib['stroke-linecap'], 'round')
            self.assertEqual(svg.attrib['stroke-linejoin'], 'round')
        arrays = re.findall(r'uint8_t (\w+)\[\] = \{([^}]+)\}', HEADER.read_text())
        self.assertEqual(len(arrays), 12)
        payload = 0
        unique = set()
        for name, body in arrays:
            size = int(name.rsplit('_', 1)[1])
            self.assertIn(size, (40, 56))
            bits = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', body))
            stride = (size + 7) // 8
            self.assertEqual(len(bits), stride * size)
            payload += len(bits)
            ink = {(x, y) for y in range(size) for x in range(size)
                   if not bits[y * stride + x // 8] & (0x80 >> (x % 8))}
            self.assertGreater(len(ink), size * size // 20)
            self.assertLess(len(ink), size * size // 3)
            self.assertTrue(all(2 <= x < size - 2 and 2 <= y < size - 2 for x, y in ink))
            if size % 8:
                padding_mask = (1 << (8 - size % 8)) - 1
                self.assertTrue(all(bits[y * stride + stride - 1] & padding_mask == padding_mask
                                    for y in range(size)))
            unique.add(bits)
        self.assertEqual(len(unique), 12)
        self.assertEqual(payload, 6 * (5 * 40 + 7 * 56))

    @unittest.skipUnless(sharp_available(), 'regeneration requires developer Node.js + sharp')
    def test_regeneration_is_deterministic_and_matches_resource(self):
        with tempfile.TemporaryDirectory(prefix='ricky-nav-icons-') as directory:
            output = Path(directory) / 'icons.h'
            command = [NODE, str(ROOT / 'scripts/build_rickyos_navigation_icons.cjs'), '--out', str(output)]
            subprocess.run(command, check=True)
            self.assertEqual(output.read_bytes(), HEADER.read_bytes())
            first = output.read_bytes()
            subprocess.run(command, check=True)
            self.assertEqual(output.read_bytes(), first)

    def test_product_routing_pixels_orientation_and_tab_hit_regions(self):
        source = SOURCE.read_text()
        # The product tab bar uses the 40 px set in both UI density profiles.
        size = 40
        pill = ''.join(re.findall(r'constexpr int kPill\w+ = ricky::ds::layout::kTabPill\w+;\n', source))
        self.assertEqual(pill.count('kPill'), 2)
        for high_dpi in (False, True):
            with self.subTest(high_dpi=high_dpi):
                program = r'''
#include <cassert>
#include <cstdint>
#include "activities/MainTab.h"
#include "components/icons/rickyNavigationIcons.h"
constexpr int kIconSize=SIZE;
enum Color : uint8_t {White=1,Black=0x10};
constexpr int N=SIZE+60;  // room for the capsule, which is wider than the icon
struct GfxRenderer {
  mutable signed char pixels[N][N]{};  // 0 untouched, 1 ink, -1 paper
  int orientation=0;
  void drawPixel(int x,int y,bool ink) const {
    assert(x>=0 && x<N && y>=0 && y<N);
    int px=x,py=y;
    if(orientation==1) {px=N-1-y;py=x;}
    if(orientation==2) {px=N-1-x;py=N-1-y;}
    if(orientation==3) {px=y;py=N-1-x;}
    pixels[py][px]=ink?1:-1;
  }
  int pixel(int x,int y) const {
    if(orientation==1) return pixels[x][N-1-y];
    if(orientation==2) return pixels[N-1-y][N-1-x];
    if(orientation==3) return pixels[N-1-x][y];
    return pixels[y][x];
  }
  void fillRect(int x,int y,int w,int h,bool ink)const {
    for(int row=y;row<y+h;++row) for(int col=x;col<x+w;++col) drawPixel(col,row,ink);
  }
  void fillRoundedRect(int x,int y,int w,int h,int r,Color c)const {
    assert(r==h/2 && c==Color::Black);
    fillRect(x,y,w,h,true);
  }
};
// No 16-level frame in these checks: the anti-aliased twin never draws.
namespace RickyAaIcons {
inline bool draw(const GfxRenderer&,const uint8_t*,int,int,int,int,bool) { return false; }
}
''' + pill + method(source, 'const uint8_t* iconForTab(') + method(source, 'void drawInxIcon(') + method(source, 'void drawSelectedInxIcon(') + r'''
int main() {
  // Apps sits in the middle: it is opened more often than Storage.
  const uint8_t* expected[]={ricky_nav_home_SIZE,ricky_nav_library_SIZE,ricky_nav_apps_SIZE,
                            ricky_nav_storage_SIZE,ricky_nav_settings_SIZE};
  assert(iconForTab(MainTab::None)==nullptr);
  assert(iconForTab(static_cast<MainTab>(255))==nullptr);
  constexpr int ox=30,oy=30;
  for(unsigned i=0;i<MainTabs::values.size();++i) {
    auto tab=MainTabs::values[i];
    assert(iconForTab(tab)==expected[i]);
    for(int orientation=0;orientation<4;++orientation) {
      GfxRenderer renderer; renderer.orientation=orientation;
      drawInxIcon(renderer,iconForTab(tab),ox,oy);
      GfxRenderer active; active.orientation=orientation;
      drawSelectedInxIcon(active,iconForTab(tab),ox,oy,136);  // a 684 px tab cell
      const int px=ox+(SIZE-kPillWidth)/2, py=oy+(SIZE-kPillHeight)/2;
      int iconInk=0;
      for(int y=0;y<N;++y) for(int x=0;x<N;++x) {
        bool black=false;
        if(x>=ox && x<ox+SIZE && y>=oy && y<oy+SIZE)
          black=(expected[i][(y-oy)*((SIZE+7)/8)+(x-ox)/8]&(0x80U>>((x-ox)%8)))==0;
        assert(renderer.pixel(x,y)==(black?1:0));
        const bool inPill=x>=px && x<px+kPillWidth && y>=py && y<py+kPillHeight;
        // Selected: the icon is knocked out of a solid capsule, nothing outside it.
        if(black) {++iconInk;assert(active.pixel(x,y)==-1);}
        else assert(active.pixel(x,y)==(inPill?1:0));
      }
      assert(iconInk>0);
    }
    for(int width : {480,684,800,1216}) {
      const auto bounds=MainTabs::tabBounds(i,width);
      assert(MainTabs::fromX((bounds.left+bounds.right)/2,width)==tab);
      assert(bounds.right-bounds.left>=SIZE);
      if(i+1<MainTabs::values.size()) {
        const auto next=MainTabs::tabBounds(i+1,width);
        assert(next.left-bounds.right>=6);
        for(int x=bounds.right;x<next.left;++x) assert(MainTabs::fromX(x,width)==MainTab::None);
      }
    }
  }
}
'''
                defines = ('RICKYOS_PRODUCT', 'CROSSMUX_UI_PROFILE_HIGH_DPI') if high_dpi else ('RICKYOS_PRODUCT',)
                run_cpp(program.replace('SIZE', str(size)), include_dirs=(ROOT / 'src',), defines=defines)

    def test_resource_switch_does_not_change_navigation_geometry_or_allocate(self):
        source = SOURCE.read_text()
        for signature in ('const uint8_t* iconForTab(', 'void drawInxIcon(', 'void drawSelectedInxIcon('):
            body = method(source, signature)
            body = re.sub(r'//[^\n]*|/\*.*?\*/', '', body, flags=re.S)
            for allocation in ('malloc(', 'calloc(', 'new ', 'std::vector'):
                self.assertNotIn(allocation, body)
        draw = method(source, 'void InxTheme::drawMainTabBar(')
        self.assertIn('MainTabs::tabBounds', draw)
        product = draw
        self.assertIn('renderer.fillRect(rect.x, separatorY, rect.width, 1, true)', product)
        self.assertIn('navigationRows(rect, kPillHeight', product)
        self.assertEqual(product.count('renderer.drawText'), 1)  # one pass, no fake-bold overdraw
        self.assertNotIn('EpdFontFamily::BOLD', product)
        self.assertNotIn('kUnderlineHeight', product)


if __name__ == '__main__':
    unittest.main()
