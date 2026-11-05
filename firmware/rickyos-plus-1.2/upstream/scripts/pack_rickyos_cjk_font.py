#!/usr/bin/env python3
"""Compress a 2-bit fontconvert header into frequency-ordered groups, in place.

fontconvert.py --compress cuts groups in code point order. For a CJK face that
spreads the common characters over every group, so a UI screen or a book page
inflates nearly all of them. Here the groups are filled in a priority order
(cn_rickyos_group_order.txt: ASCII and punctuation, the UI strings' characters,
then Chinese text frequency; anything unlisted last, in code point order) and
each glyph records its group in a glyphToGroup table, the layout FontDecompressor
already reads. Glyph indices, intervals and metrics are unchanged; within a group
the bitmaps are concatenated in glyph order, as the decoder expects.

Every glyph's pixels are checked after decompression before the header is written.

  python3 scripts/pack_rickyos_cjk_font.py lib/EpdFont/builtinFonts/notosans_cjk_12_rickyos.h
"""
import argparse
import re
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ORDER = ROOT / 'lib/EpdFont/scripts/cn_rickyos_group_order.txt'
# Bounds the decoder's per-group PSRAM buffer; a hot slot holds one group. 16 KB keeps the
# UI's characters in about eight groups, the decoder's hot-slot count (see the sizing
# notes in docs/engineering/chinese-build.md).
DEFAULT_GROUP_BYTES = 16384


def array_body(text, pattern):
    match = re.search(pattern + r'\s*=\s*\{(.*?)\n\};', text, re.S)
    if not match:
        raise SystemExit(f'Missing array: {pattern}')
    return match


def rows(body):
    return [[int(x.strip(), 0) for x in row.split(',') if x.strip()] for row in re.findall(r'\{([^{}]+)\}', body)]


def aligned_rows(packed, width, height):
    """Packed 2-bit pixels (rows run on) -> rows padded to whole bytes, high bits first."""
    stride = (width + 3) // 4
    out = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            index = y * width + x
            pixel = (packed[index // 4] >> ((3 - index % 4) * 2)) & 3
            out[y * stride + x // 4] |= pixel << ((3 - x % 4) * 2)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('header', type=Path)
    parser.add_argument('--order', type=Path, default=DEFAULT_ORDER)
    parser.add_argument('--group-bytes', type=int, default=DEFAULT_GROUP_BYTES)
    args = parser.parse_args()

    text = args.header.read_text(encoding='utf-8')
    name = re.search(r'static const EpdFontData (\w+) = \{', text)[1]
    if ' compressed: true' in text or f'{name}Groups' in text:
        raise SystemExit(f'{args.header} is already compressed; regenerate it uncompressed first')
    if 'mode: 2-bit' not in text:
        raise SystemExit(f'{args.header}: only 2-bit faces are supported')

    bitmap_match = array_body(text, rf'static const uint8_t {name}Bitmaps\[\d+\]')
    bitmap = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', bitmap_match[1]))
    glyph_match = array_body(text, rf'static const EpdGlyph {name}Glyphs\[\]')
    glyphs = rows(glyph_match[1])
    intervals = rows(array_body(text, rf'static const EpdUnicodeInterval {name}Intervals\[\]')[1])

    codepoint = [0] * len(glyphs)
    for first, last, offset in intervals:
        for cp in range(first, last + 1):
            codepoint[offset + cp - first] = cp

    order = args.order.read_text(encoding='utf-8').rstrip('\n')
    rank = {}
    for position, ch in enumerate(order):
        rank.setdefault(ord(ch), position)
    priority = sorted(range(len(glyphs)), key=lambda i: (rank.get(codepoint[i], len(order)), codepoint[i]))

    aligned = []
    for width, height, _advance, _left, _top, length, offset in glyphs:
        packed = bitmap[offset:offset + length]
        if len(packed) != length or length != (width * height + 3) // 4:
            raise SystemExit(f'{args.header}: inconsistent glyph bitmap length')
        aligned.append(aligned_rows(packed, width, height))

    # Fill groups in priority order up to the size bound.
    group_of = [0] * len(glyphs)
    members = [[]]
    size = 0
    for index in priority:
        if members[-1] and size + len(aligned[index]) > args.group_bytes:
            members.append([])
            size = 0
        members[-1].append(index)
        group_of[index] = len(members) - 1
        size += len(aligned[index])
    if len(members) > 0xFFFF:
        raise SystemExit('too many groups for the uint16 glyphToGroup table')

    data = bytearray()
    groups = []
    new_glyphs = [list(g) for g in glyphs]
    for group, indices in enumerate(members):
        indices.sort()  # the decoder walks a group's members in glyph order
        raw = bytearray()
        packed_offset = 0
        for index in indices:
            new_glyphs[index][6] = packed_offset  # within-group offset, as fontconvert writes it
            packed_offset += glyphs[index][5]
            raw += aligned[index]
        encoder = zlib.compressobj(9, zlib.DEFLATED, -15)
        stream = encoder.compress(bytes(raw)) + encoder.flush()
        if zlib.decompress(stream, -15) != raw:
            raise SystemExit(f'group {group} does not round-trip')
        groups.append((len(data), len(stream), len(raw), len(indices), indices[0]))
        data += stream

    # Check every glyph's pixels from the stored streams, the way the decoder finds them.
    for group, (offset, length, raw_size, _count, _first) in enumerate(groups):
        raw = zlib.decompress(bytes(data[offset:offset + length]), -15)
        if len(raw) != raw_size:
            raise SystemExit(f'group {group}: wrong size')
        cursor = 0
        for index in members[group]:
            span = len(aligned[index])
            if raw[cursor:cursor + span] != aligned[index]:
                raise SystemExit(f'glyph {index} (U+{codepoint[index]:04X}) differs after decompression')
            cursor += span
        if cursor != raw_size:
            raise SystemExit(f'group {group}: leftover bytes')

    def lines(values, per_line, fmt):
        return '\n'.join('    ' + ' '.join(fmt(v) for v in values[i:i + per_line]) for i in range(0, len(values), per_line))

    bitmap_text = (f'static const uint8_t {name}Bitmaps[{len(data)}] = {{\n' + lines(list(data), 16, lambda b: f'0x{b:02X},') +
                   '\n};')
    glyph_lines = []
    for row, cp in zip(new_glyphs, codepoint):
        comment = f'U+{cp:04X}' if cp < 0x21 or cp > 0x7E or chr(cp) in '\\/' else chr(cp)
        glyph_lines.append('    { ' + ', '.join(map(str, row)) + ' }, // ' + comment)
    glyph_text = f'static const EpdGlyph {name}Glyphs[] = {{\n' + '\n'.join(glyph_lines) + '\n};'
    tables = (f'static const EpdFontGroup {name}Groups[] = {{\n' +
              '\n'.join('    { ' + ', '.join(map(str, g)) + ' },' for g in groups) + '\n};\n\n' +
              f'// Group of each glyph (frequency order, scripts/pack_rickyos_cjk_font.py).\n'
              f'static const uint16_t {name}GlyphToGroup[] = {{\n' + lines(group_of, 16, lambda v: f'{v},') + '\n};\n\n')

    text = text[:glyph_match.start()] + glyph_text + text[glyph_match.end():]
    bitmap_match = array_body(text, rf'static const uint8_t {name}Bitmaps\[\d+\]')
    text = text[:bitmap_match.start()] + bitmap_text + text[bitmap_match.end():]
    marker = f'static const EpdFontData {name} = {{'
    head, tail = text.split(marker, 1)
    # groups, groupCount, glyphToGroup follow the is2Bit flag.
    tail, count = re.subn(r'\n    true,\n    nullptr,\n    0,\n    nullptr,',
                          f'\n    true,\n    {name}Groups,\n    {len(groups)},\n    {name}GlyphToGroup,', tail, count=1)
    if count != 1:
        raise SystemExit('unexpected EpdFontData layout')
    text = head + tables + marker + tail
    text = text.replace(' * mode: 2-bit\n',
                        f' * mode: 2-bit  compressed: true (frequency groups of <= {args.group_bytes} bytes,\n'
                        f' *   packed by scripts/pack_rickyos_cjk_font.py)\n', 1)
    args.header.write_text(text, encoding='utf-8')
    table_bytes = len(groups) * 20 + len(group_of) * 2
    print(f'{name}: {len(bitmap)} -> {len(data)} bitmap bytes + {table_bytes} table bytes in {len(groups)} groups; '
          f'every glyph verified')


if __name__ == '__main__':
    main()
