"""Actual product policy: upgrades keep the designed menu and semantic gray ink."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class VisualRestorationTest(unittest.TestCase):
    def test_saved_toolbar_preference_cannot_bypass_product_menu(self):
        source = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        policy = method(source, 'bool EpubReaderActivity::usesToolbarMenu(')
        run_cpp(r'''
#include <cassert>
#include <initializer_list>
struct CrossPointSettings {enum {READER_MENU_TOOLBAR=1};int readerMenuStyle=0;};
CrossPointSettings settings;
#define SETTINGS settings
struct EpubReaderActivity {bool usesToolbarMenu() const;};
''' + policy + r'''
int main() {
  EpubReaderActivity reader;
  for(int preference:{0,1,2,255}) {
    settings.readerMenuStyle=preference;
    assert(!reader.usesToolbarMenu());
    assert(settings.readerMenuStyle==preference); // no destructive preference reset
  }
}
''')

    def test_home_secondary_metadata_and_stat_lanes_are_explicit(self):
        home = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        self.assertIn('design::secondaryText(renderer, box(profile.x, profile.y + 56', home)
        self.assertIn('design::secondaryText(renderer, box(cell.x, cover.bottom() + 52', home)
        self.assertIn('screen.takeTop(184, ds::space::k6)', home)
        self.assertIn('rickyPaw::draw(renderer, x, cell.y + 100, kPaw, week[d])', home)
        self.assertIn('constexpr int kPaw = 26;', home)
        self.assertIn('cell.y + 148', home)
        # Numeric line ends at 96, paw prints run 100-126, today's dot 130-134, caption at 148.
        self.assertGreaterEqual(100 - (40 + 56), 4)
        self.assertGreaterEqual(148 - (130 + 4), 8)

    def test_gray_adapter_is_scoped_and_no_checkerboard_or_scratch(self):
        adapter = (ROOT / 'src/components/RickyUiTarget.h').read_text()
        paint = method(adapter, 'void text(')
        self.assertIn('GfxRenderer::TextInkScope tone(renderer_, ink)', paint)
        self.assertIn('ricky::ds::gray::kInk2', paint)
        self.assertIn('ricky::ds::gray::kMute', paint)
        self.assertNotIn('drawTextDither', paint)
        self.assertNotIn('malloc', paint)
        self.assertNotIn('new ', paint)
        ui = (ROOT / 'src/components/RickyDesignUi.h').read_text()
        self.assertIn('RickyUiTarget target(renderer)', method(ui, 'inline void text('))
        self.assertIn('ds::gray::kInk2', method(ui, 'inline void secondaryText('))

    def test_compact_covers_reserve_the_title_instead_of_a_duplicate_format_tag(self):
        cover = method((ROOT / 'src/components/RickyDesignUi.h').read_text(), 'inline void cover(')
        self.assertIn('compact ? CAPTION : HEADLINE', cover)
        self.assertIn('r.height - pad * 2 - (compact ? 0 : 32)', cover)
        self.assertIn('if (!compact)', cover)
        settings = (ROOT / 'src/SettingsList.h').read_text()
        catalog = method(settings, 'inline std::vector<SettingInfo> getSettingsList(')
        self.assertIn('if (UiHighDpiProfile::enabled)', catalog)
        self.assertIn('s.nameId == StrId::STR_READER_MENU_STYLE', catalog)


if __name__ == '__main__':
    unittest.main()
