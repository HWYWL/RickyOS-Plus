"""RickyOS anti-aliased icon twins: every entry matches the 1-bpp icon it replaces."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ICONS = ROOT / 'src/components/icons'


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


class RickyAaIconsTest(unittest.TestCase):
    def test_every_twin_matches_its_one_bit_icon(self):
        arrays = {}
        for name in ('rickyAppIcons.h', 'rickyNavigationIcons.h', 'rickyPageIcons.h'):
            text = (ICONS / name).read_text()
            for match in re.finditer(r'(?:static constexpr|static const|inline constexpr) uint8_t (\w+)\[\] = \{(.*?)\};', text, re.S):
                arrays[match.group(1)] = bytes(int(b, 16) for b in re.findall(r'0x([0-9A-Fa-f]{2})', match.group(2)))
        twins = re.findall(r'\{0x([0-9A-F]{8})u,\s*(\d+),\s*(\d+),\s*k_\w+,\s*(\d+)\},\s*// (\w+)',
                           (ICONS / 'rickyAaIcons.h').read_text())
        self.assertGreaterEqual(len(twins), 24)
        for digest, width, height, _, name in twins:
            with self.subTest(icon=name):
                bits = arrays[name]
                self.assertEqual(len(bits), (int(width) + 7) // 8 * int(height))
                # A regenerated 1-bpp set without regenerating the twins would silently fall
                # back to B/W icons; this catches it.
                self.assertEqual(fnv1a(bits), int(digest, 16))


if __name__ == '__main__':
    unittest.main()
