"""RickyOS 12pt CJK face: the shared common set plus GB2312 and Big5 level 1, product-only."""
import hashlib
import os
from pathlib import Path
import re
import importlib.util
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'lib/EpdFont/scripts'
FONTS = ROOT / 'lib/EpdFont/builtinFonts'
HEADER = FONTS / 'notosans_cjk_12_rickyos.h'
SOURCE = FONTS / 'source/NotoSansSC/NotoSansSC-Regular.otf'


def han(text):
    return [c for c in text if 0x4E00 <= ord(c) <= 0x9FFF]


def intervals(header_text, name):
    body = re.search(name + r'Intervals\[\] = \{(.*?)\};', header_text, re.S).group(1)
    return [(int(a, 16), int(b, 16)) for a, b, _ in re.findall(r'\{ (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+) \}', body)]


class RickyCjkCoverageTest(unittest.TestCase):
    def test_charset_is_common_set_plus_gb2312_and_big5_level_one(self):
        common = (SCRIPTS / 'cn_common_chars.txt').read_text(encoding='utf-8').strip()
        gb2312 = han((SCRIPTS / 'gb2312_lv1.txt').read_text(encoding='utf-8'))
        big5 = han((SCRIPTS / 'big5_lv1.txt').read_text(encoding='utf-8'))
        self.assertEqual(len(gb2312), 3755)
        self.assertEqual(len(big5), 5401)
        # The vendored Big5 list is exactly level 1 (A440-C67E) of Python's codec.
        expected = []
        for lead in range(0xA4, 0xC7):
            for trail in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
                if not 0xA440 <= (lead << 8 | trail) <= 0xC67E:
                    continue
                try:
                    character = bytes([lead, trail]).decode('big5')
                except UnicodeDecodeError:
                    continue
                if 0x4E00 <= ord(character) <= 0x9FFF:
                    expected.append(character)
        self.assertEqual(big5, expected)
        # RickyOS keeps Big5 level 1 minus its 300 least frequent extra characters.
        selected = han((SCRIPTS / 'big5_rickyos_chars.txt').read_text(encoding='utf-8'))
        self.assertEqual(selected, [c for c in big5 if c in set(selected)])  # Big5 order, a subset
        self.assertEqual(len(set(big5) - set(selected)), 300)
        self.assertFalse((set(big5) - set(selected)) & (set(common) | set(gb2312)))
        product = (SCRIPTS / 'cn_rickyos_chars.txt').read_text(encoding='utf-8')
        self.assertTrue(product.startswith(common))
        extra = product[len(common):]
        self.assertEqual(len(extra), len(set(extra)))
        self.assertEqual(set(extra), (set(gb2312) | set(selected)) - set(common))
        for character in '亨們體廣壓產國':
            self.assertIn(character, extra)

    def test_header_has_a_glyph_for_every_character(self):
        text = HEADER.read_text(encoding='utf-8')
        ranges = intervals(text, 'notosans_cjk_12_rickyos')
        covered = lambda cp: any(a <= cp <= b for a, b in ranges)
        for character in (SCRIPTS / 'cn_rickyos_chars.txt').read_text(encoding='utf-8'):
            self.assertTrue(covered(ord(character)), f'missing glyph for {character}')
        # The shared 12pt face is unchanged and still lacks the added characters.
        shared = intervals((FONTS / 'notosans_cjk_common_intervals.h').read_text(encoding='utf-8'),
                           'notosans_cjk_common')
        self.assertFalse(any(a <= ord('亨') <= b for a, b in shared))

    def test_missing_punctuation_draws_as_a_look_alike(self):
        # Chinese books set the ellipsis as U+22EF, which no built-in face (and few TTFs)
        # carries: it drew as boxes. Glyph lookup falls back to a look-alike that the
        # built-in faces do carry.
        source = (ROOT / 'lib/EpdFont/EpdFont.cpp').read_text(encoding='utf-8')
        table = source[source.index('uint32_t lookAlike(const uint32_t cp) {'):]
        table = table[:table.index('\n}\n')]
        for missing, stand_in in ((0x22EF, 0x2026), (0x2E3A, 0x2014), (0x2E3B, 0x2014), (0x30FB, 0x00B7)):
            case = table.index('case 0x%04X:' % missing)
            self.assertEqual(int(table[table.index('return 0x', case) + 9:][:4], 16), stand_in, hex(missing))
        lookup = source[source.index('const EpdGlyph* EpdFont::getGlyph(const uint32_t cp, bool* const usedReplacement)'):]
        self.assertLess(lookup.index('lookAlike(cp)'), lookup.index('*usedReplacement = true;'))
        self.assertIn('return alt != 0 && hasCodepoint(alt);', source)
        header = (FONTS / 'notosans_cjk_12_rickyos.h').read_text(encoding='utf-8')
        intervals = re.search(r'Intervals\[\] = \{(.*?)\n\};', header, re.S).group(1)
        spans = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),', intervals)]
        for stand_in in (0x2026, 0x2014, 0x00B7):
            self.assertTrue(any(a <= stand_in <= b for a, b in spans), hex(stand_in))

    def test_every_chinese_ui_character_is_in_the_ui_subset(self):
        # The 14-18 pt UI faces carry only the characters of the Chinese strings. A new
        # string with one character outside them draws the whole label from a fallback
        # face (it showed as slanted bold), so cn_i18n_chars.txt must cover every string.
        subset = (SCRIPTS / 'cn_i18n_chars.txt').read_text(encoding='utf-8')
        strings = (ROOT / 'lib/I18n/translations/chinese.yaml').read_text(encoding='utf-8')
        missing = sorted({c for c in strings if 0x4E00 <= ord(c) <= 0x9FFF and c not in subset})
        self.assertEqual(missing, [], 'regenerate with lib/EpdFont/scripts/build-cn-builtin-fonts.sh')

    def test_only_the_high_density_product_uses_the_extended_face(self):
        main = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        block = main[main.index('#include <builtinFonts/notosans_cjk_12_rickyos.h>'):]
        block = block[:block.index('EpdFontFamily offlineReaderFontFamily')]
        self.assertIn('#include <builtinFonts/notosans_cjk_12_rickyos.h>', block)
        self.assertIn('cjk12Data = notosans_cjk_12_rickyos', block)
        self.assertIn('EpdFont offlineReaderFont(&cjk12Data);', main)
        self.assertIn('EpdFont cjk12Font(&cjk12Data);', main)
        self.assertNotIn('notosans_cjk_12_rickyos', (FONTS / 'all.h').read_text(encoding='utf-8'))

    @unittest.skipUnless(SOURCE.exists() and importlib.util.find_spec('fontTools') and importlib.util.find_spec('freetype'),
                         'regeneration needs the gitignored Noto Sans CJK SC source and font build deps')
    def test_regeneration_reproduces_the_committed_header(self):
        script = (SCRIPTS / 'build-rickyos-cjk-font.sh').read_text(encoding='utf-8')
        expected = re.search(r'SOURCE_SHA256="([0-9a-f]{64})"', script).group(1)
        self.assertEqual(hashlib.sha256(SOURCE.read_bytes()).hexdigest(), expected)
        before = HEADER.read_bytes(), (SCRIPTS / 'cn_rickyos_chars.txt').read_bytes()
        try:
            subprocess.run(['bash', str(SCRIPTS / 'build-rickyos-cjk-font.sh')], check=True, capture_output=True,
                           env={**os.environ, 'PYTHON': sys.executable})
            self.assertEqual(HEADER.read_bytes(), before[0])
            self.assertEqual((SCRIPTS / 'cn_rickyos_chars.txt').read_bytes(), before[1])
        finally:
            HEADER.write_bytes(before[0])
            (SCRIPTS / 'cn_rickyos_chars.txt').write_bytes(before[1])


if __name__ == '__main__':
    unittest.main()
