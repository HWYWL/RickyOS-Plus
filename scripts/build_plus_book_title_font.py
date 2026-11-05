#!/usr/bin/env python3
"""Build a real Medium face at BODY metrics with the existing BODY repertoire.

Noto CJK Sans Simplified Chinese Medium, SIL OFL 1.1 (license in the restored
scripts/fonts/NotoSansSC-OFL.txt). Source: notofonts/noto-cjk,
Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Medium.otf. The pinned digest matches
the project's existing Medium UI face. Normal builds use the recorded header.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
APP = PORT / 'firmware/rickyos-plus-1.2/upstream'
NAME = 'ricky_book_12_medium'
SOURCE_SHA = 'ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path, default=PORT/'.local/source-fonts/NotoSansSC-Medium.otf')
    args = parser.parse_args()
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != SOURCE_SHA:
        raise SystemExit('Medium font source hash mismatch; never substitute another weight')
    source = APP/'lib/EpdFont/builtinFonts/notosans_cjk_12_rickyos.h'
    body = source.read_text()
    table = body.split('notosans_cjk_12_rickyosIntervals[] = {', 1)[1].split('};', 1)[0]
    ranges = re.findall(r'\{\s*0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),', table)
    characters = ''.join(sorted(set(chr(cp) for lo, hi in ranges for cp in range(int(lo,16), int(hi,16)+1)) | {'\ufffd'}))
    assert characters and '\ufffd' in characters and '\0' not in characters
    # Same 2-bit glyph coverage as the existing full-repertoire BODY. A full
    # 4-bit copy consumes the OTA reserve; keep true Medium and whole-line
    # coverage instead of truncating the charset or altering the partition.
    command = [sys.executable, 'lib/EpdFont/scripts/fontconvert.py', NAME, '12', str(args.font.resolve()),
               '--2bit', '--characters', characters, '--line-height', '36', '--ascender', '29']
    result = subprocess.run(command, capture_output=True, cwd=APP)
    if result.returncode:
        sys.stderr.write(result.stderr.decode(errors='replace')[-6000:])
        raise SystemExit('Book-title font conversion failed')
    output = b'\n'.join(line.rstrip() for line in result.stdout.splitlines())+b'\n'
    # Legacy 2-bit conversion omits EpdFontData's trailing optional handlers.
    # Make them explicit for this new face so strict application/host builds
    # keep warning checks enabled. All are absent for a built-in bitmap face.
    end = output.rfind(b'};')
    assert end > output.index(f'static const EpdFontData {NAME}'.encode())
    output = output[:end]+b'    nullptr,\n'*5+b'    false,  // is4Bit: BODY-compatible 2-bit antialias coverage\n'+output[end:]
    # Avoid leaking a developer's absolute source path into the recorded header.
    output = output.replace(str(args.font.resolve()).encode(), b'NotoSansSC-Medium.otf')
    header = APP/f'lib/EpdFont/builtinFonts/{NAME}.h'
    header.write_bytes(output)
    print(f'{NAME}: {len(characters)} codepoints; Medium at 12pt, 36px line/29px ascender; {len(output)} header bytes')


if __name__ == '__main__':
    main()
