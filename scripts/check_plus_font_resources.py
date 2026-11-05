#!/usr/bin/env python3
"""Reject stale packed UI fonts before compiling a misleading Plus image."""
import hashlib
import json
from pathlib import Path

FACES = (
    'notosans_cjk_12_rickyos', 'notosans_cjk_14', 'notosans_cjk_16',
    'ricky_caption_10_regular', 'ricky_display_23_bold',
    'ricky_headline_14_medium', 'ricky_title_16_bold', 'ricky_book_12_medium',
)


def verify(project):
    report = project / 'generated/fonts/packing-report.json'
    records = json.loads(report.read_text())
    names = [record['name'] for record in records]
    if len(names) != len(FACES) or set(names) != set(FACES):
        raise ValueError('Packed font report does not cover the eight Plus faces exactly once')
    stale = []
    for record in records:
        name = record['name']
        source = project / 'upstream/lib/EpdFont/builtinFonts' / f'{name}.h'
        packed = project / 'generated/fonts' / f'{name}.packed.h'
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if digest != record.get('source_sha256') or not packed.is_file() or not packed.stat().st_size:
            stale.append(name)
    if stale:
        raise ValueError('Stale/missing packed Plus fonts: ' + ', '.join(stale))


if __name__ == '__main__':
    project = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2'
    try:
        verify(project)
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(f'{error}\nRun scripts/prepare_rickyos_plus_resources.sh with the resource Python first.')
    print('Packed Plus fonts match all eight restored source faces')
