"""RickyOS default avatars: animal line-art sources, generated resources and storage."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'src/components/icons/rickyAvatars.h'
SCRIPT = ROOT / 'scripts/build_rickyos_avatars.py'
SOURCES = ROOT / 'src/images/sources/rickyos/avatars'
# Settings store "@avatar:<index>": this order is persistent and must never change.
ORDER = ['dog', 'cat', 'rabbit', 'bear', 'panda', 'fox', 'owl', 'penguin', 'bichon']


class RickyAvatarTest(unittest.TestCase):
    def test_nine_animals_in_persistent_order(self):
        text = HEADER.read_text()
        self.assertIn('inline constexpr const uint8_t* ALL[] = {' + ', '.join(ORDER) + '};', text)
        self.assertIn('inline constexpr int COUNT = 9;', text)
        self.assertIn('inline constexpr int SIZE = 160;', text)
        for name in ORDER:
            self.assertTrue((SOURCES / f'{name}.png').exists(), name)
            body = re.search(rf'uint8_t {name}\[\] = \{{([^}}]+)\}}', text).group(1)
            data = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', body))
            # Run-length coded 4-bit coverage: the runs cover the whole 160x160 picture.
            self.assertEqual(sum((byte >> 4) + 1 for byte in data), 160 * 160, name)
            ink = sum(((byte >> 4) + 1) * (byte & 0x0F) for byte in data) / 15
            self.assertTrue(2000 < ink < 16000, name)  # line art, not a blank or a filled disc
            self.assertLess(len(data), 6000, name)  # about what a 1-bit copy would take

    def test_cat_theme_default_avatar(self):
        text = HEADER.read_text()
        self.assertTrue((SOURCES / 'theme-cat.png').exists())
        self.assertIn('inline constexpr uint8_t catDefault[] = {', text)
        self.assertIn('inline constexpr uint16_t CAT_DEFAULT_BYTES = ', text)
        profile = (ROOT / 'src/components/RickyProfile.cpp').read_text()
        self.assertIn('index == 0 && SETTINGS.rickyCat()', profile)
        home = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        self.assertIn('SETTINGS.rickyCompanion', home)  # the cached home avatar follows the theme

    def test_regeneration_matches_checked_in_header(self):
        with tempfile.TemporaryDirectory(prefix='ricky-avatars-') as directory:
            output = Path(directory) / 'avatars.h'
            subprocess.run([sys.executable, str(SCRIPT), '--out', str(output)], check=True, capture_output=True)
            self.assertEqual(output.read_bytes(), HEADER.read_bytes())

    def test_avatar_setting_and_drawing(self):
        profile = (ROOT / 'src/components/RickyProfile.cpp').read_text()
        self.assertIn('constexpr char PRESET_PREFIX[] = "@avatar:";', profile)
        self.assertIn('return index < RickyAvatars::COUNT ? index : -1;', profile)
        # No avatar chosen yet: the puppy, never the brand badge (the brand stays on boot/standby).
        self.assertIn("if (SETTINGS.rickyAvatarPath[0] == '\\0') return DEFAULT_AVATAR;", profile)
        self.assertNotIn('drawBrandAvatar', profile)
        # Area-sampled like the brand badge, never enlarged past the source.
        self.assertIn('const int drawn = std::min(size, SOURCE);', profile)
        # 16-level frames grade the edges by coverage; B/W keeps the 7/16 ink threshold.
        self.assertIn('if (coverage) blendInk(renderer, x0 + x, y0 + y, coverage);', profile)
        self.assertIn('total * 16 >= 7 * 15 * area', profile)
        self.assertIn('return setAvatar(target);', profile)  # imports share the rollback path
        picker = (ROOT / 'src/activities/settings/RickyAvatarPickerActivity.cpp').read_text()
        self.assertIn('snprintf(value, sizeof(value), "@avatar:%d", index)', picker)
        self.assertIn('FileBrowserActivity::Mode::PickAvatar', picker)
        self.assertIn('int listCount() const override { return RickyAvatars::COUNT + 1; }',
                      (ROOT / 'src/activities/settings/RickyAvatarPickerActivity.h').read_text())


if __name__ == '__main__':
    unittest.main()
