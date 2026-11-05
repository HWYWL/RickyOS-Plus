"""Exercise production chrome drawing and maintained-SVG generation."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts/tests'))
import test_reading_ui_regressions as harness
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'
harness.ROOT = ROOT
from test_builtin_font_shrink import rows

HEADER = ROOT / 'src/components/icons/uiChromeIcons.h'


def arrays(text):
    return {name: bytes(int(value, 16) for value in re.findall(r'0x([0-9A-Fa-f]{2})', body))
            for name, body in re.findall(r'uint8_t\s+(\w+)\[\]\s*=\s*\{([^}]+)\}', text)}


class UiChromeIconTest(unittest.TestCase):
    def test_native_dimensions_and_white_padding(self):
        data = arrays(HEADER.read_text())
        self.assertEqual(len(data), 11)
        for name, bits in data.items():
            size = 56 if name.endswith('_56_bits') else 48 if name.endswith('_48_bits') else 32 if name.endswith('_32_bits') else 24 if name.endswith('_24_bits') else None
            self.assertEqual(len(bits), size * ((size + 7) // 8) if size else 80)
            if name.endswith('_56_bits'):
                self.assertEqual(bits[:7], b'\xff' * 7)
                self.assertEqual(bits[-7:], b'\xff' * 7)
            self.assertTrue(any(byte != 255 for byte in bits))

    @unittest.skipUnless(shutil.which('rsvg-convert'), 'optional SVG generation requires rsvg-convert')
    def test_regeneration_matches_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'chrome.h'
            subprocess.run([sys.executable, str(ROOT / 'scripts/build_ui_chrome_icons.py'),
                            '--out', str(output)], check=True)
            self.assertEqual(arrays(output.read_text()), arrays(HEADER.read_text()))
            first = output.read_bytes()
            subprocess.run([sys.executable, str(ROOT / 'scripts/build_ui_chrome_icons.py'),
                            '--out', str(output)], check=True)
            self.assertEqual(first, output.read_bytes())
            for manifest, header in (('listIcons', 'listIcons48'), ('readerToolbarIcons', 'readerToolbarIcons48')):
                command = [sys.executable, str(ROOT / 'freeink-sdk/libs/assets/Icons/tools/gen_icons.py'),
                           '--manifest', str(ROOT / f'src/components/icons/{manifest}.manifest'),
                           '--svgdir', str(ROOT / 'src/components/icons/sources/lucide'),
                           '--sizes', '48', '--out', str(output)]
                subprocess.run(command, check=True)
                self.assertEqual(arrays(output.read_text()),
                                 arrays((ROOT / f'src/components/icons/{header}.h').read_text()))
                first = output.read_bytes()
                subprocess.run(command, check=True)
                self.assertEqual(first, output.read_bytes())

    def test_footer_descenders_and_estimate_fit_the_status_line(self):
        faces = {}
        directory = ROOT / 'lib/EpdFont/builtinFonts'
        for name in ('notosans_12_regular', 'notosans_14_regular', 'notosans_cjk_12', 'ubuntu_12_regular'):
            source = (directory / f'{name}.h').read_text()
            metadata = source.split(f'static const EpdFontData {name} = {{')[1].split('};')[0].split(',')
            advance, ascender = int(metadata[4]), int(metadata[5])
            glyphs = rows(source, 'Glyphs')
            if name == 'notosans_cjk_12':
                source += (directory / 'notosans_cjk_common_intervals.h').read_text()
            intervals = rows(source, 'Intervals')
            lookup = {cp: glyphs[offset + cp - first] for first, last, offset in intervals
                      for cp in range(first, last + 1)}
            faces[name] = advance, ascender, lookup
        advance = faces['notosans_12_regular'][0]
        for name, chars in (('notosans_12_regular', 'gypjqQ'), ('notosans_cjk_12', '中阅读梦海'),
                            ('ubuntu_12_regular', 'אבג')):
            height, ascender, glyphs = faces[name]
            offset = int((advance - height) / 2)  # C++ truncates toward zero.
            for char in chars:
                glyph = glyphs[ord(char)]
                self.assertLessEqual(offset + ascender - glyph[4] + glyph[1], advance, char)
        estimate_advance, estimate_ascender, estimate_glyphs = faces['notosans_14_regular']
        marker = estimate_glyphs[ord('~')]
        marker_bottom = int((advance - estimate_advance) / 2) + estimate_ascender - marker[4] + marker[1]
        self.assertLessEqual(marker_bottom, advance)

    def test_reader_footer_geometry_with_all_status_fields(self):
        source = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text()
        layout_source = (ROOT / 'src/components/UITheme.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
struct Rect { int x,y,width,height; };
constexpr int READER_STATUS_FONT_ID=1, READER_ESTIMATE_FONT_ID=2;
struct CrossPointSettings {
 enum STATUS_BAR_PROGRESS_BAR { BOOK_PROGRESS=1 };
 enum { STATUS_BAR_CLOCK_HIDE=0,
        STATUS_BAR_CLOCK_LEFT=1, STATUS_BAR_CLOCK_RIGHT=2 };
 struct Spec {
   bool showBookProgressPercent=true,showChapterPageCount=true,showBattery=true,showBatteryPercent=true;
   int clockMode=STATUS_BAR_CLOCK_LEFT,progressBarMode=BOOK_PROGRESS,progressBarHeightPx=4;
   bool clock12h=false;
   bool showsProgressBar() const {return progressBarMode!=0;}
   bool showsClock() const {return clockMode!=0;}
   bool textLaneVisible() const {return true;}
   bool headerLaneVisible() const {return true;}
   bool footerLaneVisible() const {return true;}
 } spec;
 Spec statusBarSpec() const {return spec;}
} SETTINGS;
struct { uint16_t getBatteryPercentage() const {return 100;} } powerManager;
namespace bleinput { bool connected=false; bool isConnected() {return connected;} }
namespace TimeUtils { bool formatCurrentTime(char* text,size_t,bool) { std::strcpy(text,"23:59"); return true; } }
class GfxRenderer {
 public:
 int width=684,height=1216,top=5,right=5,bottom=8,left=5;
 int footerLineHeight=34;
 mutable std::vector<Rect> texts;
 int getScreenWidth() const {return width;} int getScreenHeight() const {return height;}
 void getOrientedViewableTRBL(int* t,int* r,int* b,int* l) const {*t=top;*r=right;*b=bottom;*l=left;}
 int getLineHeight(int font) const {return font==READER_ESTIMATE_FONT_ID?40:font==READER_STATUS_FONT_ID?footerLineHeight:34;}
 int getTextWidth(int font,const char* text) const {return std::strlen(text)*(font==READER_ESTIMATE_FONT_ID?20:16);}
 void drawText(int font,int x,int y,const char* text) const {
   if (!*text) return;
   // The larger estimate face only draws '~' (32px ascender, 13px top, 5px ink height).
   Rect rect{x,font==READER_ESTIMATE_FONT_ID?y+19:y,getTextWidth(font,text),
             font==READER_ESTIMATE_FONT_ID?5:getLineHeight(font)};
   assert(x>=left+14 && x+rect.width<=width-right-14);
   assert(y>=height-bottom-58 && y+rect.height<=height-bottom-5);
   for (auto old : texts) assert(rect.x+rect.width<=old.x || old.x+old.width<=rect.x);
   texts.push_back(rect);
 }
 std::string truncatedText(int,const char*,int maxWidth) const {
   assert(maxWidth>0); return std::string(maxWidth/16,'L');
 }
 void fillRect(int x,int y,int w,int h,bool=true) const {
   assert(x>=0 && x+w<=width && y>=0 && y+h<=height);
 }
};
// Numeric status face keeps its established +8px offset; the actual font-aware
// helper is verified separately by test_plus_battery_alignment.py.
namespace ricky::battery {
inline int iconTop(const GfxRenderer&,int,int textY,int) {return textY+8;}
}
struct Metrics { int batteryWidth=32,batteryHeight=20,statusBarHorizontalMargin=14,statusBarVerticalMargin=48; };
struct BaseTheme {
 static constexpr int STATUS_NUMERIC_FONT_ID=3,batteryPercentSpacing=4;
 void drawBatteryLeft(const GfxRenderer&,Rect,bool) const;
 static void drawBatteryOutline(const GfxRenderer& r,int x,int y,int w,int h) {
   assert(w==32 && h==20 && x>=r.left+14 && x+w<=r.width-r.right-14);
   assert(y>=r.height-r.bottom-58 && y+h<=r.height-r.bottom-5);
 }
 void fillBatteryIcon(const GfxRenderer&,Rect,uint16_t) const {}
 static void drawStatusBar(GfxRenderer&,float,int,int,std::string,int,int,bool,bool,bool);
};
struct UITheme {
 static UITheme& getInstance() {static UITheme theme;return theme;}
 Metrics getMetrics() const {return {};}
 BaseTheme& getTheme() {static BaseTheme theme;return theme;}
 int getStatusBarHeight() const {return 48+10;}
 static int getStatusBarTextTopPadding(const GfxRenderer&);
};
#define GUI UITheme::getInstance().getTheme()
constexpr int bookmarkStatusIconWidth=24,bookmarkStatusIconGap=12,bluetoothStatusIconWidth=24;
void drawBookmarkStatusIcon(const GfxRenderer& r,int x,int y) {
 assert(x>=r.left+14 && x+24<=r.width-r.right-14 && y+24<=r.height-r.bottom-5);
}
void drawBluetoothStatusIcon(const GfxRenderer& r,int x,int y) {drawBookmarkStatusIcon(r,x,y);}
''' + method(layout_source, 'int UITheme::getStatusBarTextTopPadding(')
            + method(source, 'void BaseTheme::drawBatteryLeft(')
            + method(source, 'void BaseTheme::drawStatusBar(') + r'''
int main() {
 for (auto size : {Rect{0,0,684,1216},Rect{0,0,600,1000}})
 for (int orientation=0;orientation<4;++orientation) for (bool percent : {false,true})
 for (int clock : {CrossPointSettings::STATUS_BAR_CLOCK_HIDE,CrossPointSettings::STATUS_BAR_CLOCK_LEFT,
                   CrossPointSettings::STATUS_BAR_CLOCK_RIGHT}) for(bool markers : {false,true})
 for(int footerLineHeight : {26,34,42}) {
   GfxRenderer r;
   r.footerLineHeight=footerLineHeight;
   r.width=size.width;r.height=size.height;
   if (orientation%2) {r.width=size.height;r.height=size.width;}
   if (orientation==1) {r.top=5;r.right=8;r.bottom=5;r.left=5;}
   if (orientation==2) {r.top=8;r.right=5;r.bottom=5;r.left=5;}
   if (orientation==3) {r.top=5;r.right=5;r.bottom=5;r.left=8;}
   SETTINGS.spec.showBatteryPercent=percent;SETTINGS.spec.clockMode=clock;bleinput::connected=markers;
   BaseTheme::drawStatusBar(r,100,65535,65535,std::string(200,'L'),0,0,false,markers,true);
   assert(!r.texts.empty());
   assert(r.texts[1].y+std::max(footerLineHeight,34)==r.height-r.bottom-10-UiHighDpiProfile::readerStatusBottomPadding);
 }
}
''', defines=('CROSSMUX_UI_PROFILE_HIGH_DPI','FREEINK_DEVICE_READPICO=1'))

    def test_reader_content_reserves_product_chrome_and_preserves_legacy_geometry(self):
        layout_source = (ROOT / 'src/components/UITheme.cpp').read_text()
        reader_source = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        # renderBook and the pickers share textMargins(); compile its body for the
        # Original reading mode, which must keep the product chrome geometry.
        margins = method(reader_source, 'readingMode::Margins EpubReaderActivity::textMargins(')
        body = margins[margins.index('{') + 1:margins.rindex('}')]
        program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
[[maybe_unused]] constexpr int READER_STATUS_FONT_ID=1;
[[maybe_unused]] constexpr int SMALL_FONT_ID=2;
struct BaseTheme { static constexpr int STATUS_NUMERIC_FONT_ID=3; };
struct Metrics { int statusBarVerticalMargin=UiHighDpiProfile::enabled?48:19, progressBarMarginTop=1; };
using ThemeMetrics=Metrics;
#include "activities/reader/ReaderChrome.h"
#include "components/icons/rickyReaderDog.h"
namespace RickyDesignUi {constexpr int CAPTION=10;}
struct Settings {
 uint8_t marginTop=25, marginBottom=25, marginLeft=25, marginRight=25;
 uint8_t readingMode=0;
 struct Spec {
   bool text=true, progress=false;
   int progressBarHeightPx=4;
   bool textLaneVisible() const {return text;}
   bool headerLaneVisible() const {return text;}
   bool footerLaneVisible() const {return text;}
   bool showsProgressBar() const {return progress;}
 } spec;
 Spec statusBarSpec() const {return spec;}
 int getReaderFontId() const {return 1;}
 float getReaderLineCompression() const {return 1.0f;}
} SETTINGS;
class GfxRenderer {
 public:
 int width=684,height=1216,top=5,right=5,bottom=8,left=5;
 int textHeight=34, numericHeight=34;
 int getLineHeight(int font) const {return font==BaseTheme::STATUS_NUMERIC_FONT_ID?numericHeight:textHeight;}
 int getLineHeight(int font,float) const {return getLineHeight(font);}
 int getScreenWidth() const {return width;} int getScreenHeight() const {return height;}
 void getOrientedViewableTRBL(int* t,int* r,int* b,int* l) const {*t=top;*r=right;*b=bottom;*l=left;}
};
struct UITheme {
 static UITheme& getInstance() {static UITheme theme;return theme;}
 Metrics getMetrics() const {return {};}
 static int getStatusBarHeight();
 static int getProgressBarHeight();
 static int getStatusBarTextTopPadding(const GfxRenderer&);
};
''' + method(layout_source, 'int UITheme::getStatusBarHeight(') + method(layout_source, 'int UITheme::getProgressBarHeight(') + method(layout_source, 'int UITheme::getStatusBarTextTopPadding(') + r'''
readingMode::Margins textMargins(const GfxRenderer& renderer,bool automaticPageTurnActive) {
''' + body + r'''
}
int contentBottom(const GfxRenderer& renderer,bool automaticPageTurnActive) {
 return renderer.height-textMargins(renderer,automaticPageTurnActive).bottom;
}
int main() {
 for (int orientation=0;orientation<4;++orientation)
 for (int textHeight : {26,34,42,48,56}) for (int numericHeight : {34,50})
 for (bool text : {false,true}) for (bool progress : {false,true})
 for (bool automatic : {false,true}) for (uint8_t margin : {0,25,100}) {
   GfxRenderer r;
   if(orientation%2) {r.width=1216;r.height=684;}
   if(orientation==1) {r.right=8;r.bottom=5;}
   if(orientation==2) {r.top=8;r.bottom=5;}
   if(orientation==3) {r.left=8;r.bottom=5;}
   r.textHeight=textHeight;r.numericHeight=numericHeight;
   SETTINGS.marginTop=SETTINGS.marginBottom=SETTINGS.marginLeft=SETTINGS.marginRight=margin;SETTINGS.spec.text=text;SETTINGS.spec.progress=progress;
   const int lane=UITheme::getInstance().getMetrics().statusBarVerticalMargin;
   const int bar=progress?5:0;
   const int oldReserve=(text||automatic?lane:0)+bar;
   const int oldBottom=r.height-r.bottom-std::max<int>(margin,oldReserve);
   const int bottom=contentBottom(r,automatic);
   assert(bottom<=r.height-r.bottom-margin);
   const bool productChrome=UiHighDpiProfile::enabled && (!automatic||text);
   const auto chrome=readerChrome::compactLayout(std::max(textHeight,numericHeight),text,text,progress,4);
   const int reserve=productChrome?chrome.footerReserve:oldReserve;
   assert(bottom==r.height-r.bottom-std::max<int>(margin,reserve));
   if(!productChrome) assert(bottom==oldBottom);
   const auto m=textMargins(r,automatic);
   assert(m.left==r.left+margin && m.right==r.right+margin);
   const int topReserve=UiHighDpiProfile::enabled && text?chrome.headerReserve:0;
   assert(m.top==r.top+std::max<int>(margin,topReserve));
   if (productChrome && text) {
     const int oldTop=r.top+margin+48+16;
     const int oldProductBottom=r.height-r.bottom-std::max<int>(margin,std::max(oldReserve,96));
     assert(r.height-m.top-m.bottom >= oldProductBottom-oldTop);
     if(margin==25) assert(r.height-m.top-m.bottom > oldProductBottom-oldTop);
   }
 }
}
'''
        for defines in (('CROSSMUX_UI_PROFILE_HIGH_DPI=1', 'FREEINK_DEVICE_READPICO=1'),
                        ('CROSSMUX_UI_PROFILE_HIGH_DPI=0', 'FREEINK_DEVICE_READPICO=0')):
            run_cpp(program, include_dirs=(ROOT / "src",), defines=defines)


if __name__ == '__main__':
    unittest.main()
