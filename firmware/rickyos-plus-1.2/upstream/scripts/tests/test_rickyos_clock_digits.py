"""RickyOS Standby clock digits: generated header matches its generator and font."""
import importlib.util
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class RickyClockDigitsTest(unittest.TestCase):
    def test_header_matches_generator(self):
        spec = importlib.util.spec_from_file_location('gen', ROOT / 'scripts/gen_ricky_clock_digits.py')
        gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen)
        header = (ROOT / 'src/components/fonts/rickyClockDigits.h').read_text()
        cap = int(header.split('kCapHeight = ')[1].split(';')[0])
        cell, glyphs = gen.render(ROOT / 'scripts/fonts/InterDisplay-Light.ttf', cap)
        # CI formats every header, so compare against the formatted generator output.
        clang_format = shutil.which('clang-format') or str(ROOT / '.venv/bin/clang-format')
        formatted = subprocess.run([clang_format, '--assume-filename=rickyClockDigits.h'],
                                   input=gen.header(cap, cell, glyphs), capture_output=True, text=True,
                                   check=True, cwd=ROOT).stdout
        self.assertEqual(header, formatted)
        self.assertEqual(set(glyphs), set('0123456789:'))
        # The runs decode back to every pixel of every glyph.
        for glyph in glyphs.values():
            decoded = []
            for byte in gen.runs(glyph['levels']):
                decoded += [byte & 0x0F] * ((byte >> 4) + 1)
            self.assertEqual(decoded, glyph['levels'])
        # The font ships with its licence (SIL OFL 1.1).
        self.assertIn('SIL Open Font License', (ROOT / 'scripts/fonts/Inter-LICENSE.txt').read_text())

    def test_standby_picture_clock_is_drawn_into_the_gray_frame(self):
        face = (ROOT / 'src/activities/apps/standby/WallpaperFace.cpp').read_text()
        start = face.index('  if (intoGray) {')
        block = face[start:face.index('    return;\n  }', start)]
        self.assertIn('RickyClockDigits::fadeToPaper', block)
        self.assertIn('RickyClockDigits::draw', block)
        self.assertIn('copyBwToGrayscale16', block)


if __name__ == '__main__':
    unittest.main()
