#!/usr/bin/env python3
"""Write the glyph order the 12pt CJK face is packed in (cn_rickyos_group_order.txt).

The 12pt face is the UI body font as well as the offline reader font, and its
compressed groups are filled in this order (pack_rickyos_cjk_font.py):

  0. ASCII and the common Chinese punctuation, which nearly every screen and page uses;
  1. every character of the Chinese UI strings, most frequent first, so a UI
     screen touches a handful of groups that stay in the decompressor's hot slots;
  2. every other character by Chinese text frequency, so a book page needs few groups.

Characters missing here are packed last, in code point order. The output is
checked in, so building the font needs no wordfreq; rerun this only when the UI
strings change a lot (a stale order costs speed, never correctness).

Run with the font development venv (wordfreq required):
  .venv/bin/python scripts/build_rickyos_cjk_group_order.py
"""
from collections import Counter
from pathlib import Path

import wordfreq

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'lib/EpdFont/scripts'
OUTPUT = SCRIPTS / 'cn_rickyos_group_order.txt'
FIRST = ''.join(map(chr, range(0x20, 0x7F))) + '，。、：；！？“”‘’（）《》「」…—·％'


def is_cjk(ch):
    return 0x3000 <= ord(ch) <= 0x303F or 0x4E00 <= ord(ch) <= 0x9FFF or 0xFF00 <= ord(ch) <= 0xFFEF


def main():
    frequency = Counter()
    for word, value in wordfreq.get_frequency_dict('zh').items():
        for ch in word:
            if is_cjk(ch):
                frequency[ch] += value
    ui = {ch for ch in (ROOT / 'lib/I18n/translations/chinese.yaml').read_text(encoding='utf-8') if is_cjk(ch)}
    font = {ch for ch in (SCRIPTS / 'cn_rickyos_chars.txt').read_text(encoding='utf-8') if is_cjk(ch)}
    ranked = sorted((ui | font) - set(FIRST), key=lambda ch: (ch not in ui, -frequency[ch], ord(ch)))
    OUTPUT.write_text(FIRST + ''.join(ranked) + '\n', encoding='utf-8')
    print(f'{OUTPUT.relative_to(ROOT)}: {len(ranked)} characters, {len(ui)} from the UI strings first')


if __name__ == '__main__':
    main()
