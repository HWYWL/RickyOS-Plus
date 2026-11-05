"""RickyOS Standby & power-off page: styles move to Standby, power-off keeps three screens."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text()


class RickyStandbyPowerOffTest(unittest.TestCase):
    def test_power_off_screen_follows_standby(self):
        body = method(read('src/CrossPointSettings.cpp'), 'uint8_t CrossPointSettings::rickySleepScreenMode() const')
        program = r'''
#include <cassert>
#include <cstdint>
struct CrossPointSettings {
  enum { DARK = 0, LIGHT, CUSTOM, COVER, BLANK, COVER_CUSTOM, QUICK_RESUME, TRANSPARENT };
  enum { RICKY_STANDBY_PICTURE = 0, RICKY_STANDBY_CALENDAR, RICKY_STANDBY_COVER, RICKY_STANDBY_KEEP_PAGE, RICKY_STANDBY_TRANSPARENT };
  enum { RICKY_POWER_OFF_SAME = 0, RICKY_POWER_OFF_DEFAULT, RICKY_POWER_OFF_BLANK };
  uint8_t sleepScreen = LIGHT, rickyStandbyFace = 0, rickyPowerOffScreen = 0;
  uint8_t rickySleepScreenMode() const;
};
''' + body + r'''
int main() {
  using S = CrossPointSettings;
  S s;
  s.rickyPowerOffScreen = S::RICKY_POWER_OFF_SAME;
  s.rickyStandbyFace = S::RICKY_STANDBY_PICTURE; assert(s.rickySleepScreenMode() == S::CUSTOM);
  s.rickyStandbyFace = S::RICKY_STANDBY_COVER; assert(s.rickySleepScreenMode() == S::COVER_CUSTOM);
  s.rickyStandbyFace = S::RICKY_STANDBY_KEEP_PAGE; assert(s.rickySleepScreenMode() == S::QUICK_RESUME);
  s.rickyStandbyFace = S::RICKY_STANDBY_TRANSPARENT; assert(s.rickySleepScreenMode() == S::QUICK_RESUME);
  // A stopped clock must not look like Standby: the rest screen instead.
  s.rickyStandbyFace = S::RICKY_STANDBY_CALENDAR; assert(s.rickySleepScreenMode() == S::LIGHT);
  s.rickyPowerOffScreen = S::RICKY_POWER_OFF_BLANK; assert(s.rickySleepScreenMode() == S::BLANK);
  s.rickyPowerOffScreen = S::RICKY_POWER_OFF_DEFAULT; assert(s.rickySleepScreenMode() == S::LIGHT);
  s.sleepScreen = S::DARK; assert(s.rickySleepScreenMode() == S::DARK);
}
'''
        run_cpp(program, defines=('RICKYOS_PRODUCT',))

    def test_old_sleep_screen_choice_moves_to_standby_once(self):
        source = read('src/CrossPointSettings.cpp')
        start = source.index('if (doc["rickyPowerOffScreen"].isNull()) {')
        migration = source[start:source.index('} else {', start)]
        self.assertIn('rickyStandbyFace = RICKY_STANDBY_COVER;', migration)
        self.assertIn('rickyStandbyFace = RICKY_STANDBY_KEEP_PAGE;', migration)
        self.assertIn('rickyPowerOffScreen = RICKY_POWER_OFF_BLANK;', migration)
        self.assertIn('needsResave = true;', migration)
        self.assertIn('doc["rickyPowerOffScreen"] = rickyPowerOffScreen;', source)

    def test_page_has_two_tabs_and_style_dependent_rows(self):
        page = read('src/activities/apps/standby/RickyStandbySettingsActivity.cpp')
        rows = method(page, 'void RickyStandbySettingsActivity::rebuildRows()')
        off = rows[:rows.index('return;')]
        self.assertIn('add(PowerOffScreen);', off)
        self.assertIn('add(AutoPowerOff);', off)
        self.assertNotIn('add(Style);', off)
        # Picking and downloading only for the picture; fit, filter and corner for picture and cover.
        self.assertIn('if (SETTINGS.rickyStandbyFace == Settings::RICKY_STANDBY_PICTURE) {', rows)
        self.assertIn('if (showsPicture()) {', rows)
        self.assertIn('buildTabBar(screen);', page)
        header = read('src/activities/apps/standby/RickyStandbySettingsActivity.h')
        self.assertIn('class RickyStandbySettingsActivity final : public UiTabListActivity', header)
        for language in ('chinese', 'english'):
            strings = read(f'lib/I18n/translations/{language}.yaml')
            for key in ('STR_RICKY_TAB_STANDBY', 'STR_RICKY_TAB_POWER_OFF', 'STR_RICKY_POWERED_OFF',
                        'STR_RICKY_STANDBY_STYLE_COVER', 'STR_RICKY_STANDBY_STYLE_KEEP', 'STR_RICKY_MINUTES_N'):
                self.assertRegex(strings, rf'(?m)^{key}: "')

    def test_standby_faces(self):
        standby = read('src/activities/apps/standby/StandbyActivity.cpp')
        self.assertIn('return makeUniqueNoThrow<KeepPageFace>();', standby)
        keep = read('src/activities/apps/standby/KeepPageFace.cpp')
        self.assertIn('renderer.displayBuffer(HalDisplay::FAST_REFRESH);', keep)
        self.assertNotIn('clearScreen', keep)
        face = read('src/activities/apps/standby/WallpaperFace.cpp')
        enter = method(face, 'void WallpaperFace::onEnter()')
        self.assertIn('currentCoverBmp()', enter)
        self.assertIn('path_ = kPicture;', enter)  # no book: the picture
        native = method(face, 'bool WallpaperFace::renderNative(')
        self.assertIn('filtered()', native)  # a filter is drawn B/W
        sleep = read('src/activities/boot_sleep/SleepActivity.cpp')
        self.assertIn('drawRickyStandbyIndicator(renderer, true)', sleep)
        self.assertNotIn('SETTINGS.sleepScreen ==', sleep)


if __name__ == '__main__':
    unittest.main()
