"""Execute native brand geometry and guard product hierarchical navigation."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_reading_ui_regressions import method

ROOT = Path(__file__).resolve().parents[2]


class RickyBrandNavigationTest(unittest.TestCase):
    def test_brand_geometry_and_complete_artwork(self):
        program = r'''
#include <cassert>
#include <cstdlib>
#include "components/RickyBrandMark.h"
int main() {
  for(auto box : {Rect{20,20,180,180},Rect{20,20,240,100},Rect{20,20,90,200}}) {
    auto mark=RickyBrandMark::fit(box);
    assert(mark.x>=box.x && mark.y>=box.y);
    assert(mark.x+mark.width<=box.x+box.width);
    assert(mark.y+mark.height<=box.y+box.height);
    assert(mark.width<=RickyLogoAsset::width && mark.height<=RickyLogoAsset::height);
    assert(std::abs(mark.width*RickyLogoAsset::height-mark.height*RickyLogoAsset::width)<RickyLogoAsset::height);
    int previous=0;
    for(int phase=0;phase<4;++phase) {
      int count=0;
      for(int y=0;y<RickyLogoAsset::height;++y) for(int x=0;x<RickyLogoAsset::width;++x) {
        bool pixel=RickyBrandMark::pixel(x,y,phase);
        if(phase) assert(!RickyBrandMark::pixel(x,y,phase-1) || pixel);
        if(phase==3) assert(pixel==((RickyLogoAsset::bits[y*(RickyLogoAsset::width/8)+x/8]&(0x80U>>(x%8)))==0));
        if(y>=RickyLogoAsset::wordmarkTop && phase<3) assert(!pixel);
        count+=pixel;
      }
      assert(count>previous); previous=count;
    }
    assert(previous>20000);
  }
}
'''
        with tempfile.TemporaryDirectory(prefix="ricky-brand-") as directory:
            source, binary = Path(directory)/"check.cpp", Path(directory)/"check"
            source.write_text(program)
            subprocess.run(["c++", "-std=c++20", "-I"+str(ROOT/"src"),
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_settings_product_paths_bypass_accordion(self):
        source = (ROOT/"src/activities/settings/SettingsActivity.cpp").read_text()
        activate = method(source, "void SettingsActivity::activateIndex(")
        product = activate
        self.assertIn("openRickyCategory(index)", product)
        self.assertNotIn("toggleAccordionCategory", product)
        back = method(source, "bool SettingsActivity::handleButtons(")
        self.assertIn("backToRickyCategories()", back)
        child = method(source, "void SettingsActivity::openRickyCategory(")
        self.assertIn("categoryNav_ = nav", child)
        self.assertIn("categoryRoot_ = false", child)
        parent = method(source, "void SettingsActivity::backToRickyCategories(")
        self.assertIn("nav = categoryNav_", parent)
        self.assertIn("categoryRoot_ = true", parent)
        screen = method(source, "void SettingsActivity::buildScreen(")
        product = screen
        self.assertIn("screen.list(productProps)", product)
        self.assertNotIn("toggleAccordionCategory", product)

    def test_inx_product_child_headers_include_shared_touch_back(self):
        source = (ROOT/"src/components/themes/inx/InxTheme.cpp").read_text()
        header = method(source, "void InxTheme::drawHeader(")
        self.assertIn("if (backButton && gpio.hasTouch())", header)
        self.assertIn("BaseTheme::drawHeader(renderer, rect, title, subtitle, true)", header)

    def test_local_font_manager_has_no_automatic_network(self):
        source = (ROOT/"src/activities/settings/FontLibraryActivity.cpp").read_text()
        enter = method(source, "void FontLibraryActivity::onEnter(")
        self.assertNotIn("FontDownloadActivity", enter)
        self.assertNotIn("fetch", enter)
        self.assertIn("sdFontSystem.refreshIfDirty()", enter)
        self.assertLess(enter.index("RenderLock"), enter.index("UiListActivity::onEnter"))

    def test_ricky_ui_chinese_glyphs_exist_in_embedded_fallback(self):
        text = (ROOT/"lib/I18n/translations/chinese.yaml").read_text()
        product = "\n".join(line for line in text.splitlines() if line.startswith("STR_RICKY_"))
        required = {ord(c) for c in product if "\u4e00" <= c <= "\u9fff"}
        header = (ROOT/"lib/EpdFont/builtinFonts/notosans_cjk_common_intervals.h").read_text()
        intervals = re.search(r"Intervals\[\] = \{(.*?)\n\};", header, re.S).group(1)
        codepoints = set()
        for first,last in re.findall(r"\{\s*(0x[0-9A-F]+),\s*(0x[0-9A-F]+),", intervals):
            codepoints.update(range(int(first,16), int(last,16)+1))
        self.assertFalse(required-codepoints, "".join(chr(c) for c in sorted(required-codepoints)))


if __name__ == "__main__":
    unittest.main()
