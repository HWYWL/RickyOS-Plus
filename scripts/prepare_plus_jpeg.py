#!/usr/bin/env python3
"""Derive a private Plus JPEGDEC build without mutating shared dependency files.

The existing pinned library computes cc >> (iBitNum-5) for short Huffman
codes (iBitNum 1..4). Those cannot have five leading ones; guard before shifting.
Original Apache-2.0 sources/license remain intact beside the derived source.
"""
from pathlib import Path
import hashlib
import shutil

PORT = Path(__file__).resolve().parents[1]
SOURCE = PORT / '.local/dependencies/JPEGDEC/src'
OUTPUT = PORT / '.local/jpegdec-plus'
HASHES = {
    'JPEGDEC.cpp': 'cce4a7cd6fe8fe26825276f9c65ed9c96bf1fe31a15c79e0169ee2385543d210',
    'jpeg.inl': 'b469c582cb1a0de82e3b02393ab3a831e4869856ff7f5026c977409bac24f674',
}


def main():
    inputs = {name: (SOURCE/name).read_bytes() for name in HASHES}
    for name, data in inputs.items():
        if hashlib.sha256(data).hexdigest() != HASHES[name]:
            raise SystemExit(f'JPEGDEC input differs from the audited Plus pin: {name}')
    before = b'if ((cc >> (iBitNum-5)) == 0x1f)'
    after = b'if (iBitNum >= 5 && (cc >> (iBitNum-5)) == 0x1f)'
    if inputs['jpeg.inl'].count(before) != 2:
        raise SystemExit('Expected exactly two audited JPEG Huffman guards')
    inputs['jpeg.inl'] = inputs['jpeg.inl'].replace(before, after)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, data in inputs.items():
        target = OUTPUT/name
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    shutil.copyfile(SOURCE.parent/'LICENSE', OUTPUT/'LICENSE')
    print('Plus private JPEGDEC: pinned source verified, both short-code shifts guarded')


if __name__ == '__main__':
    main()
