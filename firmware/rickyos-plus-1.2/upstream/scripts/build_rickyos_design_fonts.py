#!/usr/bin/env python3
"""Generate maintained 1.2 UI faces; never substitute synthetic weights.

Source: notofonts/noto-cjk Sans/OTF/SimplifiedChinese, SIL OFL 1.1.
Place the pinned OTFs under lib/EpdFont/builtinFonts/source/NotoSansSC.
Run with the font development venv (freetype-py and fontTools required).
Generated headers are checked in so normal firmware builds need no font tools.
"""
import hashlib
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / 'lib/EpdFont/builtinFonts'
SOURCES = FONTS / 'source/NotoSansSC'
PINNED = {
    'Regular': '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b',
    'Bold': 'b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a',
    'Medium': 'ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7',
}

def ui_characters():
    text = (ROOT / 'lib/I18n/translations/chinese.yaml').read_text()
    # Include every translated character, ASCII, punctuation and a replacement
    # glyph. Fontconvert itself rejects missing required glyphs.
    return ''.join(sorted(set(text) | set(map(chr, range(32, 127))) |
                          set('，。！？：；、（）「」《》“”‘’·…—–›‹←→↑↓％\ufffd')))

def main():
    chars = ui_characters()
    for weight, digest in PINNED.items():
        path = SOURCES / f'NotoSansSC-{weight}.otf'
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            sys.exit(f'Unexpected source font: {path}')
    faces = [('ricky_caption_10_regular', 10, 'Regular', chars, 28, 22),
             ('ricky_title_16_bold', 16, 'Bold', chars, 44, 34),
             ('ricky_headline_14_medium', 14, 'Medium', chars, 40, 32),
             ('ricky_display_23_bold', 23, 'Bold', '0123456789月日.:%％/- ', 56, 47)]
    for name, size, weight, subset, height, ascender in faces:
        # These are generated resources, not hand-edited font bytes.
        result = subprocess.run([sys.executable,
            'lib/EpdFont/scripts/fontconvert.py', name, str(size),
            str((SOURCES / f'NotoSansSC-{weight}.otf').relative_to(ROOT)), '--4bit',
            '--characters', subset, '--line-height', str(height), '--ascender', str(ascender)],
            check=True, capture_output=True, cwd=ROOT)
        # The echoed --characters argument contains YAML newlines; normalize
        # comment whitespace without touching any generated font data.
        output = b'\n'.join(line.rstrip() for line in result.stdout.splitlines()) + b'\n'
        (FONTS / f'{name}.h').write_bytes(output)
        print(f'{name}: {len(subset)} characters, {len(output)} header bytes')

if __name__ == '__main__':
    main()
