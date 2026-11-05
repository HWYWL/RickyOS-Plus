"""RickyOS 1.1.1 reader fixes: tap turns, the text-panel AA switch, battery, index errors."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text()


class RickyQuickWinsTest(unittest.TestCase):
    def test_tap_turns_are_the_default_and_migrate_the_old_swipe_only_default_once(self):
        header = read('src/CrossPointSettings.h')
        start = header.index('  // Tapping the side of the page')
        self.assertIn('uint8_t pageTurnGesture = TAP_AND_SWIPE;', header[start:start + 200])
        source = read('src/CrossPointSettings.cpp')
        start = source.index('if (doc["rickyTapTurn"].isNull())')
        migration = source[start:source.index('if (doc["rickyStandbyKeyOn"]', start)]
        # Only the untouched old default moves; any other choice stays the reader's.
        self.assertIn('pageTurnGesture == SWIPE_ONLY && previousPageGesture == SWIPE_ONLY', migration)
        self.assertIn('needsResave = true;', migration)
        self.assertIn('doc["rickyTapTurn"] = 1;', source)

    def test_every_touch_gesture_and_key_lives_on_its_own_page(self):
        source = read('src/activities/settings/SettingsActivity.cpp')
        start = source.index('const auto elsewhere = [](const SettingInfo& setting) {')
        block = source[start:source.index('for (auto* list', start)]
        for field in ('touchReaderControls', 'pageTurnGesture', 'previousPageGesture', 'showReaderMenu',
                      'pageTurnDirection', 'sideButtonLayout', 'longPressButtonBehavior', 'standbyShortcutEnabled',
                      'shortPwrBtn', 'longPressMenuFunction'):
            self.assertIn(f'&CrossPointSettings::{field}', block)
        for action in ('SdFirmwareUpdate',):
            self.assertIn(f'SettingAction::{action}', block)
        self.assertIn('SettingAction::RickyGestures', source)
        self.assertIn('SettingAction::RickyKeys', source)
        self.assertIn('RickyOptionLists::gestures()', source)
        self.assertIn('RickyOptionLists::keys()', source)
        page = read('src/activities/settings/RickyOptionListActivity.cpp')
        for field in ('rickyGestureBack', 'rickyGestureHome', 'rickyGestureControl', 'rickyGestureStatus',
                      'touchReaderControls', 'pageTurnGesture', 'previousPageGesture', 'pageTurnDirection',
                      'showReaderMenu', 'sideButtonLayout', 'longPressButtonBehavior', 'standbyShortcutEnabled'):
            self.assertIn(f'&SETTINGS.{field}', page)
        # The side key is fixed: a short press is Standby, a hold is power-off (main.cpp).
        self.assertNotIn('&SETTINGS.shortPwrBtn', page)
        # Profile is the card on top of Settings, not a second row in System.
        self.assertNotIn('SettingInfo::Action(StrId::STR_RICKY_PROFILE', source)

    def test_global_gestures_follow_their_switches(self):
        settings = read('src/CrossPointSettings.cpp')
        # Swiping up from the bottom to go Home starts off, for new and existing settings.
        self.assertIn('rickyGestureHome = (doc["rickyGestureHome"] | 0) ? 1 : 0;', settings)
        self.assertIn('uint8_t rickyGestureHome = 0;', read('src/CrossPointSettings.h'))
        mapped = read('src/MappedInputManager.cpp')
        home = mapped[mapped.index('bool MappedInputManager::wasHomeGesture() const {'):]
        self.assertLess(home.index('if (!SETTINGS.rickyGestureHome) return false;'), home.index('wasBottomEdgeUpSwipe()'))
        self.assertIn('if (!SETTINGS.rickyGestureBack) return false;', mapped)
        self.assertIn('if (!SETTINGS.rickyGestureControl) return false;', mapped)
        menu = mapped[mapped.index('bool MappedInputManager::wasReaderMenuSwipeUp() const {'):]
        self.assertIn('if (!SETTINGS.rickyGestureHome) return wasBottomEdgeUpSwipe();', menu[:400])
        self.assertIn('SETTINGS.rickyGestureStatus', read('src/activities/ActivityManager.cpp'))

    def test_text_panel_toggles_anti_aliasing_without_repagination(self):
        source = read('src/activities/reader/EpubReaderActivity.cpp')
        self.assertIn('StrId::STR_TEXT_AA', source[source.index('constexpr StrId kTextRowNames[]'):])
        start = source.index('else if (panelIndex == kTextRowAntiAliasing)')
        toggle = source[start:source.index('else {', start)]
        self.assertIn('SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;', toggle)
        self.assertNotIn('applyReaderTextSettings', toggle)

    def test_battery_header_matches_its_generator(self):
        spec = importlib.util.spec_from_file_location('gen', ROOT / 'scripts/gen_ricky_battery_icons.py')
        gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen)
        self.assertEqual(read('src/components/icons/rickyBatteryIcons.h'), gen.header())
        # The bar the firmware fills stays clear of the body outline.
        body = gen.body()
        self.assertFalse(any((x, y) in body for x in range(4, 24) for y in range(5, 15)))
        self.assertFalse(gen.bolt() & gen.halo())

    def test_index_failures_name_their_cause_and_log_to_the_card(self):
        source = read('src/activities/reader/EpubReaderActivity.cpp')
        for key in ('MEMORY', 'DATA', 'IO'):
            self.assertIn(f'STR_RICKY_INDEX_FAILED_{key}', source)
        for lang in ('chinese', 'english'):
            strings = read(f'lib/I18n/translations/{lang}.yaml')
            for key, code in (('MEMORY', 'E1'), ('DATA', 'E2'), ('IO', 'E3')):
                line = next(l for l in strings.splitlines() if l.startswith(f'STR_RICKY_INDEX_FAILED_{key}:'))
                self.assertIn(code, line)
        self.assertIn('appendRickyErrorLog(epub->getPath(), currentSpineIndex, stage', source)
        self.assertIn('kRickyErrorLogMaxBytes', source)


if __name__ == '__main__':
    unittest.main()
