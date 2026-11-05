#!/usr/bin/env python3
"""Select the Big5 level-1 hanzi the RickyOS 12pt face carries.

Big5 level 1 (big5_lv1.txt) adds 2890 Traditional characters beyond the
Simplified common set and GB2312 level 1. RickyOS keeps all but the DROP least
frequent of those, ranked by wordfreq's Zipf score for Chinese, to hold the
app-slot reserve above 512 KiB. Output keeps Big5 order and is committed, so
font regeneration never depends on the installed wordfreq version.

Usage (font build venv, see docs/engineering/chinese-build.md):
    python3 select_rickyos_big5.py   # writes big5_rickyos_chars.txt
"""
from pathlib import Path

import wordfreq

DROP = 300
HERE = Path(__file__).resolve().parent


def han(text):
    return [c for c in text if 0x4E00 <= ord(c) <= 0x9FFF]


def main():
    have = set(han((HERE / 'cn_common_chars.txt').read_text(encoding='utf-8')))
    have |= set(han((HERE / 'gb2312_lv1.txt').read_text(encoding='utf-8')))
    big5 = han((HERE / 'big5_lv1.txt').read_text(encoding='utf-8'))
    extra = [c for c in big5 if c not in have]
    # Least frequent first; ties broken by Big5 order so the cut is deterministic.
    ranked = sorted(range(len(extra)), key=lambda i: (wordfreq.zipf_frequency(extra[i], 'zh'), -i))
    dropped = {extra[i] for i in ranked[:DROP]}
    keep = [c for c in big5 if c not in dropped]
    (HERE / 'big5_rickyos_chars.txt').write_text(''.join(keep), encoding='utf-8')
    print(f'big5_rickyos_chars.txt: {len(keep)} of {len(big5)} Big5 level-1 characters '
          f'({len(extra) - DROP} beyond the Simplified sets, {DROP} least frequent dropped)')


if __name__ == '__main__':
    main()
