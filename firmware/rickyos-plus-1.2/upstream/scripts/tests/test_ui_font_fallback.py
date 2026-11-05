"""Execute production fallback selection, setup and unload with small font/I/O seams."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class UiFontFallbackTest(unittest.TestCase):
    def test_high_dpi_whole_run_coverage(self):
        from test_builtin_font_shrink import rows
        directory = ROOT / 'lib/EpdFont/builtinFonts'
        for size in (12, 14, 16):
            source = (directory / f'notosans_cjk_{size}.h').read_text()
            if size == 12:
                source += (directory / 'notosans_cjk_common_intervals.h').read_text()
            coverage = {cp for first, last, _ in rows(source, 'Intervals')
                        for cp in range(first, last + 1)}
            self.assertIn(ord('海'), coverage)
            self.assertEqual(ord('梦') in coverage, size == 12)
        renderer = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <set>
#include <string>
#define LOG_ERR(...) ((void)0)
namespace BidiUtils { enum class BidiBaseDir { AUTO }; }
const void* measured=nullptr;
const char* resolveVisualText(const char* text,std::string& visual,BidiUtils::BidiBaseDir) {
  if (std::string(text)=="ا") { visual="ﺍ";return visual.c_str(); }
  return text;
}
#include "Utf8.h"
#include "Utf8.cpp"
struct EpdFontFamily {
  enum Style { REGULAR, BOLD };
  std::set<uint32_t> coverage;
  bool hasCodepoint(uint32_t cp, Style) const { return coverage.contains(cp); }
  void getTextDimensions(const char*,int* w,int* h,Style) const { measured=this;*w=20;*h=20; }
};
struct GfxRenderer {
  std::map<int,EpdFontFamily> fontMap;
  std::map<int,std::array<int,2>> fallbackFontMap_;
  std::map<int,int> preferredFontMap_;
  void ensureSdGlyphsResident(int,const char*,EpdFontFamily::Style,bool) const {}
  int getTextWidth(int,const char*,EpdFontFamily::Style=EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir=BidiUtils::BidiBaseDir::AUTO) const;
  int resolveFontFamilyId(int) const;
  int resolveTextFontId(int,const char*,EpdFontFamily::Style=EpdFontFamily::REGULAR) const;
};
''' + method(renderer, 'int GfxRenderer::resolveFontFamilyId(')
            + method(renderer, 'int GfxRenderer::resolveTextFontId(')
            + method(renderer, 'int GfxRenderer::getTextWidth(') + r'''
int main() {
  GfxRenderer r;
  r.fontMap[14]={{'A'}}; // NotoSans
  r.fontMap[114]={{'A',0x6d77}}; // CJK UI subset has 海, but not 梦
  r.fontMap[12]={{'A',0x6d77,0x68a6}}; // common CJK
  r.fontMap[212]={{'A',0x05d0,0xfe8d}}; // Ubuntu Hebrew / shaped Arabic
  r.fallbackFontMap_[14]={114,0};
  r.fallbackFontMap_[114]={12,0};
  r.fallbackFontMap_[12]={212,0};
  for(auto style : {EpdFontFamily::REGULAR,EpdFontFamily::BOLD}) {
    assert(r.resolveTextFontId(14,"海",style)==114);
    assert(r.resolveTextFontId(14,"梦海",style)==12);
    assert(r.resolveTextFontId(14,"A梦海",style)==12);
    assert(r.resolveTextFontId(14,"א",style)==212);
    assert(r.resolveTextFontId(14,"ﺍ",style)==212);
    assert(r.resolveTextFontId(14,"A",style)==14);
  }
  assert(r.getTextWidth(14,"ا")==20);
  assert(measured==&r.fontMap.at(212)); // selection sees shaped Arabic, as drawing does
  r.fontMap[414]={{'A',0x6d77,0x68a6,0x9f98}}; // complete SD fallback
  r.fallbackFontMap_[14]={114,414};
  assert(r.resolveTextFontId(14,"海")==114); // same-size CJK UI remains first
  assert(r.resolveTextFontId(14,"梦海")==414);
  assert(r.resolveTextFontId(14,"龘海")==414);
  r.fontMap.erase(414); // SD removal keeps common CJK / Ubuntu fallback
  r.fallbackFontMap_[14]={114,0};
  r.fontMap[314]={{0x6d77}}; // incomplete SD face
  r.preferredFontMap_[14]=314;
  r.fallbackFontMap_[14]={14,114};
  assert(r.resolveTextFontId(14,"A")==14);
  assert(r.resolveTextFontId(14,"梦海")==12);
  r.fallbackFontMap_[212]={14,0}; // bounded even if a chain cycles
  assert(r.resolveTextFontId(14,"龘")==314);
  r.preferredFontMap_.clear(); // unloading SD preserves fixed fallbacks
  assert(r.resolveTextFontId(14,"梦海")==12);
}
''', include_dirs=(ROOT / 'lib/Utf8',), defines=('CROSSMUX_UI_PROFILE_HIGH_DPI',))

    def test_high_dpi_reader_family_does_not_replace_ui(self):
        system = (ROOT / 'src/SdCardFontSystem.cpp').read_text()
        table = system[system.index('struct UiFontSize'):system.index('}  // namespace', system.index('struct UiFontSize'))]
        run_cpp(r'''
#define ENABLE_CHINESE_VERSION 1
#define CROSSMUX_UI_PROFILE_HIGH_DPI 1
#define SIMULATOR 1
#define FREEINK_DEVICE_READPICO 1
#define LOG_DBG(...) ((void)0)
#include <cassert>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>
constexpr int SMALL_FONT_ID=1, UI_10_FONT_ID=2, UI_12_FONT_ID=3;
constexpr int CJK_UI_12_FONT_ID=6;
constexpr int READER_STATUS_FONT_ID=7, READER_ESTIMATE_FONT_ID=8;
constexpr int CJK_UI_14_FONT_ID=14, CJK_UI_16_FONT_ID=16;
struct GfxRenderer {
  std::vector<int> preferred, fallback;
  void clearPreferredFonts() { preferred.clear(); }
  void setPreferredFont(int id,int) { preferred.push_back(id); }
  void setFallbackFont(int id,int,int) { fallback.push_back(id); }
};
struct Family {};
struct Registry { Family family; const Family* findFamily(const std::string&) { return &family; } };
struct Manager {
  std::string family="Reader A";
  std::vector<int> sizes;
  const std::string& currentFamilyName() { return family; }
  int loadFamilyExtraSize(const Family&,GfxRenderer&,int size) { sizes.push_back(size); return 100+size; }
};
struct SdCardFontSystem {
  Registry registry_; Manager manager_;
  void setupUiFallbacks(GfxRenderer&);
};
''' + table + method(system, 'void SdCardFontSystem::setupUiFallbacks(') + r'''
int main() {
  SdCardFontSystem system; GfxRenderer renderer;
  for (const char* family : {"Reader A", "Reader B"}) {
    system.manager_.family=family; system.manager_.sizes.clear(); renderer.fallback.clear();
    system.setupUiFallbacks(renderer);
    assert((system.manager_.sizes==std::vector<int>{12,14,16,12,14}));
    assert((renderer.preferred==std::vector<int>{READER_STATUS_FONT_ID,READER_ESTIMATE_FONT_ID}));
    assert((renderer.fallback==std::vector<int>{SMALL_FONT_ID,UI_10_FONT_ID,UI_12_FONT_ID,READER_STATUS_FONT_ID,READER_ESTIMATE_FONT_ID}));
  }
}
''')

    def test_high_dpi_vector_ui_keeps_builtin_backup(self):
        system = (ROOT / 'src/SdCardFontSystem.cpp').read_text()
        table = system[system.index('struct UiFontSize'):system.index('}  // namespace', system.index('struct UiFontSize'))]
        run_cpp(r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
constexpr int SMALL_FONT_ID=1,UI_10_FONT_ID=2,UI_12_FONT_ID=3;
constexpr int CJK_UI_12_FONT_ID=12,CJK_UI_14_FONT_ID=14,CJK_UI_16_FONT_ID=16;
constexpr int READER_STATUS_FONT_ID=7,READER_ESTIMATE_FONT_ID=8;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_INTERNAL=2,MALLOC_CAP_8BIT=4;
size_t heap_caps_get_largest_free_block(int) { return 1024*1024; }
size_t heap_caps_get_free_size(int) { return 1024*1024; }
struct TtfEpdFont {
  static inline int failSize=0;
  int size=0;
  bool load(int pt,bool,int,int) { size=pt;return pt!=failSize; }
  int family() const { return size; }
};
template<class T> auto makeUniqueNoThrow() { return std::make_unique<T>(); }
int computeTtfFontId(const char*,int pt) { return 100+pt; }
struct GfxRenderer {
  std::map<int,std::array<int,2>> fallback;
  std::map<int,int> fonts;
  const auto& getFontMap() const { return fonts; }
  void insertFont(int id,int size) { assert(fonts.emplace(id,size).second); }
  void registerTtfFont(int,TtfEpdFont*) {}
  void setFallbackFont(int id,int primary,int backup) { fallback[id]={primary,backup}; }
};
struct SdCardFontSystem {
  std::string ttfFamily_="SD";
  std::vector<std::unique_ptr<TtfEpdFont>> ttfUi_;
  std::vector<int> ttfUiIds_;
  void addTtfSources(TtfEpdFont&) {}
  void setupTtfUiFallbacks(GfxRenderer&);
};
''' + table + method(system, 'void SdCardFontSystem::setupTtfUiFallbacks(') + r'''
int main() {
  for(int fail : {0,16}) {
    TtfEpdFont::failSize=fail;
    SdCardFontSystem system; GfxRenderer r;
    r.fallback[UI_12_FONT_ID]={16,0};
    system.setupTtfUiFallbacks(r);
    assert((r.fallback[SMALL_FONT_ID]==std::array<int,2>{12,112}));
    assert((r.fallback[UI_10_FONT_ID]==std::array<int,2>{14,114}));
    assert((r.fallback[UI_12_FONT_ID]==std::array<int,2>{16,fail?0:116}));
    assert((r.fallback[READER_STATUS_FONT_ID]==std::array<int,2>{12,112}));
    assert((r.fallback[READER_ESTIMATE_FONT_ID]==std::array<int,2>{14,114}));
    assert(system.ttfUi_.size()==(fail?2U:3U));
    assert(system.ttfUi_.capacity()>=5 && system.ttfUiIds_.capacity()>=5);
  }
}
''', defines=('CROSSMUX_UI_PROFILE_HIGH_DPI','FREEINK_DEVICE_READPICO=1'))

    def test_high_dpi_vector_unload_preserves_fixed_ui(self):
        system = (ROOT / 'src/SdCardFontSystem.cpp').read_text()
        run_cpp(r'''
#include <cassert>
#include <string>
#include <vector>
struct GfxRenderer {
  std::vector<int> removed;
  bool fixedUiFallback=true;
  void unregisterTtfFont(int) {}
  void removeFont(int id) { removed.push_back(id); }
  void clearFallbackFonts() { fixedUiFallback=false; }
};
struct OwnedFont { bool freed=false; void reset() { freed=true; } };
struct SdCardFontSystem {
  std::string ttfFamily_="Reader";
  int ttfFontId_=114, ttfPointSize_=14;
  std::vector<int> ttfUiIds_{108,110}, ttfUi_{8,10};
  OwnedFont ttf_; bool sourcesFreed=false;
  void freeTtfSources() { sourcesFreed=true; }
  void unloadTtf(GfxRenderer&);
};
''' + method(system, 'void SdCardFontSystem::unloadTtf(') + r'''
int main() {
  GfxRenderer renderer; SdCardFontSystem system;
  system.unloadTtf(renderer);
  assert(renderer.fixedUiFallback);
  assert((renderer.removed==std::vector<int>{108,110,114}));
  assert(system.ttf_.freed && system.sourcesFreed && system.ttfFamily_.empty());
  assert(system.ttfUiIds_.empty() && system.ttfUi_.empty() && system.ttfFontId_==0);
  system.unloadTtf(renderer);
  assert(renderer.removed.size()==3);
}
''', defines=('CROSSMUX_UI_PROFILE_HIGH_DPI',))

    def test_missing_outline_geometry(self):
        renderer = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        program = r'''
#include <cassert>
#include "MissingGlyph.h"
namespace BidiUtils { bool isTransparentMark(uint32_t) { return false; } }
enum class TextRotation { None, Rotated90CW };
struct GfxRenderer {
  mutable int calls=0, x=0, y=0, w=0, h=0;
  mutable bool black=false;
  bool visible=true;
  bool glyphIntersectsStrip(int,int,int,int) const { return visible; }
  void drawRect(int px,int py,int pw,int ph,bool state) const {
    ++calls; x=px; y=py; w=pw; h=ph; black=state;
  }
};
template <TextRotation rotation = TextRotation::None>
''' + method(renderer, 'static void renderMissingGlyph(') + r'''
int main() {
  EpdFontData font{}; font.ascender=12;
  GfxRenderer r;
  renderMissingGlyph(r,font,0x1F9EA,20,30,true);
  assert(r.calls==1 && r.x==21 && r.y==21 && r.w==9 && r.h==9 && r.black);
  renderMissingGlyph(r,font,0x1F9EA,20,30,false,true);
  assert(r.calls==2 && r.x==20 && r.y==26 && r.w==5 && r.h==5 && !r.black);
  renderMissingGlyph<TextRotation::Rotated90CW>(r,font,0x1F9EA,20,30,true);
  assert(r.calls==3 && r.x==23 && r.y==21 && r.w==9 && r.h==9);
  for (uint32_t cp : {0x20,0xA0,0x200D,0xFE0F,0x301}) renderMissingGlyph(r,font,cp,20,30,true);
  assert(r.calls==3);
  r.visible=false;
  renderMissingGlyph(r,font,0x1F9EA,20,30,true);
  assert(r.calls==3);
}
'''
        with tempfile.TemporaryDirectory(prefix='missing-outline-') as directory:
            cpp = Path(directory) / 'check.cpp'
            exe = Path(directory) / 'check'
            cpp.write_text(program)
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'lib/EpdFont'), '-I', str(ROOT / 'lib/Utf8'),
                            '-I', str(ROOT / 'lib/MiniBidi'), str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)



if __name__ == '__main__':
    unittest.main()
