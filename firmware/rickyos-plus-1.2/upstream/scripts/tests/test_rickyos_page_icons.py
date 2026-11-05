"""RickyOS storage/settings icons: Lucide sources rendered to fixed 40 px 1-bpp resources."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_rickyos_navigation_icons import NODE, sharp_available

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'src/components/icons/rickyPageIcons.h'
SCRIPT = ROOT / 'scripts/build_rickyos_page_icons.cjs'


class RickyPageIconTest(unittest.TestCase):
    def test_header_holds_every_page_icon_at_40_px(self):
        text = HEADER.read_text()
        aliases = re.findall(r"\['(\w+)', '[\w-]+'\]", SCRIPT.read_text())
        self.assertEqual(len(aliases), 29)  # pages, reader toolbar and file actions
        for alias in aliases:
            body = re.search(rf'icon_ricky_{alias}_40_bits\[\] = \{{([^}}]+)\}}', text).group(1)
            data = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', body))
            self.assertEqual(len(data), 5 * 40, alias)          # 40 rows of 5 bytes
            self.assertLess(data.count(0xFF), len(data), alias)  # has ink
            center = int(re.search(rf'icon_ricky_{alias}_40 = \{{40, 40, (\d+),', text).group(1))
            self.assertTrue(8 <= center <= 32, alias)
        for page in ('src/activities/home/RickyStorageActivity.cpp', 'src/activities/settings/SettingsActivity.cpp'):
            self.assertIn('components/icons/rickyPageIcons.h', (ROOT / page).read_text())

    @unittest.skipUnless(sharp_available(), 'regeneration requires developer Node.js + sharp')
    def test_regeneration_matches_checked_in_header(self):
        with tempfile.TemporaryDirectory(prefix='ricky-page-icons-') as directory:
            output = Path(directory) / 'icons.h'
            subprocess.run([NODE, str(SCRIPT), '--out', str(output)], check=True, capture_output=True)
            self.assertEqual(output.read_bytes(), HEADER.read_bytes())

    def test_reader_rows_have_native_32_px_resources(self):
        text = HEADER.read_text()
        aliases = re.findall(r"\['(row_\w+)', '[\w-]+', 32\]", SCRIPT.read_text())
        self.assertEqual(len(aliases), 15)
        for alias in aliases:
            body = re.search(rf'icon_ricky_{alias}_32_bits\[\] = \{{([^}}]+)\}}', text).group(1)
            self.assertEqual(len(re.findall(r'0x[0-9A-F]{2}', body)), 4 * 32)


if __name__ == '__main__':
    unittest.main()
