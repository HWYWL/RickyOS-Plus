"""Production power geometry and grouped-settings safety regressions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RickyPowerSettingsTest(unittest.TestCase):
    def test_product_settings_do_not_synthetically_thicken_whole_list(self):
        source = (ROOT / "src/activities/settings/SettingsActivity.cpp").read_text()
        screen = source.split("void SettingsActivity::buildScreen(", 1)[1]
        self.assertIn("constexpr bool boldChineseCategories = false", screen)
        self.assertIn("if (category) item.state = fui::StateEmphasized", source)

    def test_power_and_six_category_geometry(self):
        program = r'''
#include <cassert>
#include <array>
#include "components/RickyPowerLayout.h"
#include "InxItemLayout.h"
int main() {
  for (const auto safe : {Rect{32, 32, 620, 1152}, Rect{32, 32, 1152, 620},
                          Rect{18, 20, 500, 750}, Rect{18, 20, 750, 380}}) {
    for (int text : {28, 34, 40}) {
      const auto m = RickyPowerLayout::fit(safe, text, 12);
      for (const auto rect : {m.mark, m.message, m.footer}) {
        assert(rect.x >= safe.x && rect.y >= safe.y);
        assert(rect.width > 0 && rect.height > 0);
        assert(rect.x + rect.width <= safe.x + safe.width);
        assert(rect.y + rect.height <= safe.y + safe.height);
      }
      assert(m.mark.y + m.mark.height + 12 <= m.message.y);
      assert(m.message.y + m.message.height + 12 <= m.footer.y);
    }
  }
  const std::array<int, 6> counts{12, 14, 8, 7, 4, 18};
  for (unsigned mask = 0; mask < 64; ++mask) {
    int total = 0;
    for (int category = 0; category < 6; ++category) {
      assert(InxAccordionGeometry::categoryRow(counts, mask, category) == total);
      const auto row = InxAccordionGeometry::rowAt(counts, mask, total++);
      assert(row.isCategory() && row.category == category);
      if (!(mask & (1 << category))) continue;
      for (int item = 0; item < counts[category]; ++item) {
        const auto child = InxAccordionGeometry::rowAt(counts, mask, total++);
        assert(child.category == category && child.setting == item);
      }
    }
    assert(InxAccordionGeometry::visibleCount(counts, mask) == total);
  }
}
'''
        with tempfile.TemporaryDirectory(prefix="ricky-power-settings-") as directory:
            source, binary = Path(directory) / "check.cpp", Path(directory) / "check"
            source.write_text(program)
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_top_status_brand_is_removed(self):
        source = (ROOT / "src/components/themes/inx/InxTheme.cpp").read_text()
        status = source.split("void InxTheme::drawMainTabStatusBar(", 1)[1].split("\n}", 1)[0]
        self.assertNotIn("STR_CROSSPOINT", status)
        self.assertIn("formatCurrentTime", status)
        self.assertIn("drawBatteryRight", status)

    def test_sleep_transition_is_bounded_and_other_modes_survive(self):
        source = (ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text()
        self.assertIn("if (!fromTimeout)", source)
        self.assertIn("drawRickyPowerScreen(renderer, true, true)", source)
        self.assertIn("drawRickyPowerScreen(renderer, true, false)", source)
        for mode in ("QUICK_RESUME", "TRANSPARENT", "BLANK", "CUSTOM", "COVER", "COVER_CUSTOM"):
            self.assertIn("SLEEP_SCREEN_MODE::" + mode, source)
        start = source.index("const bool defaultScene")
        transition = source[start:source.index("switch (sleepScreenMode())", start)]
        self.assertNotIn("delay(", transition)
        self.assertNotIn("while (", transition)

    def test_settings_lists_move_rows_without_copies(self):
        source = (ROOT / "src/activities/settings/SettingsActivity.cpp").read_text()
        # Standby and power-off screens are chosen on their own page (labels in display
        # order, stored values mapped there); Settings only links to it.
        page = (ROOT / "src/activities/apps/standby/RickyStandbySettingsActivity.cpp").read_text()
        self.assertIn("static_assert(std::size(kStyleLabels) == std::size(kStyles));", page)
        self.assertIn("std::move(*it)", source)
        self.assertIn("swap(librarySettings)", source)
        self.assertIn("swap(connectionSettings)", source)
        self.assertNotIn("STR_RICKY_HELP_OTA", source)
        self.assertIn("productProps.rowGap = std::max<int16_t>(6", source)


if __name__ == "__main__":
    unittest.main()
