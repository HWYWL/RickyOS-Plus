"""RickyOS Standby calendar clock: dates, quiet refresh, keys-left landscape."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]
FACE = ROOT / 'src/activities/apps/standby/CalendarClockFace.cpp'
STANDBY = ROOT / 'src/activities/apps/standby/StandbyActivity.cpp'


class RickyStandbyCalendarTest(unittest.TestCase):
    def test_month_grid_dates(self):
        source = FACE.read_text()
        program = '\n'.join(method(source, name) for name in (
            'bool leapYear(', 'int daysInMonth(', 'int weekday(')) + r'''
#include <cassert>
int main() {
  assert(weekday(2026, 6, 4) == 4);    // the photo: 2026-06-04 was a Thursday
  assert(weekday(2026, 10, 8) == 4);
  assert(weekday(2000, 1, 1) == 6);
  assert(weekday(2024, 2, 29) == 4);
  assert(daysInMonth(2024, 2) == 29 && daysInMonth(2026, 2) == 28 && daysInMonth(2000, 2) == 29);
  assert(daysInMonth(1900, 2) == 28 && daysInMonth(2026, 12) == 31 && daysInMonth(2026, 11) == 30);
}
'''
        run_cpp(program)

    def test_face_is_pure_bw_and_standby_turns_only_while_it_shows(self):
        header = (ROOT / 'src/activities/apps/standby/CalendarClockFace.h').read_text()
        # No renderNative: the B/W path refreshes each minute with the quiet waveform.
        self.assertNotIn('renderNative', header)
        self.assertIn('RickyClockDigits::drawBw(', FACE.read_text())
        enter = method(STANDBY.read_text(), 'void StandbyActivity::onEnter()')
        self.assertIn('savedOrientation_ = renderer.getOrientation();', enter)
        self.assertIn('renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);', enter)
        self.assertIn('renderer.setOrientation(savedOrientation_);', method(STANDBY.read_text(),
                                                                          'void StandbyActivity::onExit()'))
        self.assertIn('CrossPointSettings::RICKY_STANDBY_CALENDAR', STANDBY.read_text())

    def test_keys_left_is_landscape_clockwise(self):
        # The portrait bottom (keys) maps to the panel's right edge; LandscapeClockwise maps
        # logical x = 0 there, so the keys sit to the left of the landscape picture.
        source = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        rotate = method(source, 'static inline void rotateCoordinates(')
        portrait = rotate[rotate.index('case GfxRenderer::Portrait:'):rotate.index('case GfxRenderer::LandscapeClockwise:')]
        self.assertIn('*phyX = y;', portrait)
        clockwise = rotate[rotate.index('case GfxRenderer::LandscapeClockwise:'):]
        self.assertIn('*phyX = panelWidth - 1 - x;', clockwise[:200])


if __name__ == '__main__':
    unittest.main()
