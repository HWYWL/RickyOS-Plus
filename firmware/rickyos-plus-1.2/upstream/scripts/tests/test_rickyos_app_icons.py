"""RickyOS app-grid icons: one line system with the tab bar, native size, every product app covered."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'src/components/icons/rickyAppIcons.h'
SOURCES = ROOT / 'src/components/icons/sources/rickyos/apps'
MENU = ROOT / 'src/activities/apps/AppsMenuActivity.cpp'
NODE = os.environ.get('RICKY_ICON_NODE') or shutil.which('node')


def sharp_available():
    return NODE and subprocess.run([NODE, '-e', 'require("sharp")'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


class RickyAppIconTest(unittest.TestCase):
    def test_sources_share_the_tab_bar_line_system(self):
        for svg_path in sorted(SOURCES.glob('*.svg')):
            svg = ET.parse(svg_path).getroot()
            self.assertEqual(svg.attrib['viewBox'], '0 0 24 24', svg_path.name)
            self.assertEqual(svg.attrib['stroke-width'], '1.2', svg_path.name)
            self.assertEqual(svg.attrib['stroke-linecap'], 'round', svg_path.name)
            self.assertEqual(svg.attrib['stroke-linejoin'], 'round', svg_path.name)

    def test_native_40_px_resources_with_white_padding(self):
        arrays = re.findall(r'uint8_t ricky_app_(\w+)_40\[\] = \{([^}]+)\}', HEADER.read_text())
        self.assertEqual(sorted(name for name, _ in arrays), sorted(p.stem for p in SOURCES.glob('*.svg')))
        for name, body in arrays:
            bits = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', body))
            self.assertEqual(len(bits), 5 * 40, name)
            ink = [(x, y) for y in range(40) for x in range(40) if not bits[y * 5 + x // 8] & (0x80 >> (x % 8))]
            self.assertGreater(len(ink), 40 * 40 // 20, name)
            self.assertTrue(all(2 <= x < 38 and 2 <= y < 38 for x, y in ink), name)

    def test_every_product_app_has_a_line_icon_and_capsule_selection(self):
        source = MENU.read_text()
        catalog = source.split('constexpr AppEntry kAppEntries[] = {', 1)[1].split('};', 1)[0]
        # Entries the product build keeps: drop the stock-only #ifndef RICKYOS_PRODUCT blocks,
        # which may nest other conditionals.
        kept, skipping = [], []
        for line in catalog.splitlines():
            if line.startswith('#if'):
                skipping.append(line.startswith('#ifndef RICKYOS_PRODUCT') or any(skipping))
            elif line.startswith('#endif'):
                skipping.pop()
            elif not any(skipping):
                kept.append(line)
        product = '\n'.join(kept)
        icons = set(re.findall(r'UIIcon::(\w+)', product))
        routing = source.split('const uint8_t* rickyAppIcon(', 1)[1].split('\n}\n', 1)[0]
        self.assertEqual(icons, set(re.findall(r'case UIIcon::(\w+):', routing)))
        draw = source.split('void drawRickyAppIcon(', 1)[1].split('\n}\n', 1)[0]
        # Every app sits on a rounded-square tile; the selected one is filled with the
        # icon knocked out (B/W pass), or its anti-aliased twin on a 16-level frame.
        self.assertIn('renderer.fillRoundedRect(tileX, tileY, kAppTileSize, kAppTileSize, kAppTileRadius, Color::Black)',
                      draw)
        # Resting tiles get an exact light-gray ring (1-bit ink on a B/W frame).
        self.assertIn('RickyGrayStroke::ring(renderer, tileX, tileY, kAppTileSize, kAppTileSize, kAppTileStroke, '
                      'kAppTileRadius, ricky::ds::gray::kLine)', re.sub(r'\s+', ' ', draw))
        self.assertIn('RickyAaIcons::draw(renderer, icon, kRickyAppIconSize, kRickyAppIconSize, x, y, !selected)', draw)
        self.assertIn('renderer.drawPixel(x + column, y + row, !selected)', draw)
        grid = source.split('void AppsMenuActivity::drawIconGrid(', 1)[1].split('void AppsMenuActivity::buildScreen', 1)[0]
        self.assertIn('const int blockWidth = std::min(152, cell.width - 16)', grid)
        self.assertIn('kAppPillHeight + labelGap + 28 + 22', grid)
        self.assertNotIn('fill(renderer, labelRect', grid)

    @unittest.skipUnless(sharp_available(), 'regeneration requires developer Node.js + sharp')
    def test_regeneration_matches_resource(self):
        with tempfile.TemporaryDirectory(prefix='ricky-app-icons-') as directory:
            output = Path(directory) / 'icons.h'
            subprocess.run([NODE, str(ROOT / 'scripts/build_rickyos_app_icons.cjs'), '--out', str(output)], check=True)
            self.assertEqual(output.read_bytes(), HEADER.read_bytes())


if __name__ == '__main__':
    unittest.main()
