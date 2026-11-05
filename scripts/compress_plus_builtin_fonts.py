#!/usr/bin/env python3
"""Losslessly pack pinned 2/4-bit fonts using the existing EpdFontGroup format."""
import hashlib
import json
import re
import zlib
import zopfli.zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / 'firmware/rickyos-plus-1.2'
GROUP_LIMIT = 16384  # Bound the existing decoder's temporary PSRAM group allocation.


def array(text, declaration):
    match = re.search(declaration + r'\s*=\s*\{(.*?)\n\};', text, re.S)
    if not match:
        raise ValueError(declaration)
    return match


def build(name):
    source = PROJECT / 'upstream/lib/EpdFont/builtinFonts' / (name + '.h')
    text = source.read_text()
    output = PROJECT / 'generated/fonts' / (name + '.packed.h')
    output.parent.mkdir(parents=True, exist_ok=True)
    if f'static const EpdFontGroup {name}Groups[]' in text:
        # Design already packs this face (frequency-ordered groups with glyphToGroup);
        # keep its packing, which inflates far less per UI screen than code-point groups.
        text = '// Copied by scripts/compress_plus_builtin_fonts.py (already packed upstream). Do not edit.\n' + text
        if not output.exists() or output.read_text() != text:
            output.write_text(text)
        return {'compressor': 'upstream', 'name': name, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
    bits = 4 if 'true,  // is4Bit:' in text else 2
    pixels_per_byte = 8 // bits
    mask = (1 << bits) - 1
    bitmap_match = array(text, r'static const uint8_t ' + name + r'Bitmaps\[\d+\]')
    bitmap = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', bitmap_match[1]))
    glyph_match = array(text, r'static const EpdGlyph ' + name + r'Glyphs\[\]')
    glyphs = [[int(x.strip(), 0) for x in row.split(',') if x.strip()] for row in re.findall(r'\{([^{}]+)\}', glyph_match[1])]
    chunks, groups, rewritten = [], [], []
    aligned = bytearray()
    first = 0
    packed_offset = 0

    def flush():
        nonlocal aligned, first, packed_offset
        if first == len(rewritten):
            return
        encoder = zlib.compressobj(9, wbits=-15)
        ordinary = encoder.compress(aligned) + encoder.flush()
        # Build-time only: a denser compatible DEFLATE stream, same group RAM bound.
        cache = ROOT / '.local/fontpack-zopfli-v1'
        cache.mkdir(parents=True, exist_ok=True)
        cached = cache / (hashlib.sha256(aligned).hexdigest() + '.deflate')
        if cached.exists():
            dense = cached.read_bytes()
        else:
            dense = zopfli.zlib.compress(bytes(aligned), numiterations=5)[2:-4]
            assert zlib.decompress(dense, -15) == aligned
            cached.write_bytes(dense)
        data = dense if len(dense) < len(ordinary) else ordinary
        assert zlib.decompress(data, -15) == aligned
        groups.append([sum(map(len, chunks)), len(data), len(aligned), len(rewritten) - first, first])
        chunks.append(data)
        aligned = bytearray()
        first = len(rewritten)
        packed_offset = 0

    for glyph in glyphs:
        width, height, advance, left, top, length, offset = glyph
        assert length == (width * height + pixels_per_byte - 1) // pixels_per_byte
        packed = bitmap[offset:offset + length]
        assert len(packed) == length
        stride = (width + pixels_per_byte - 1) // pixels_per_byte
        row_data = bytearray(stride * height)
        # Same row padding as upstream fontconvert.py:814-841.
        for y in range(height):
            for x in range(width):
                index = y * width + x
                pixel = (packed[index // pixels_per_byte] >> ((pixels_per_byte - 1 - index % pixels_per_byte) * bits)) & mask
                row_data[y * stride + x // pixels_per_byte] |= pixel << ((pixels_per_byte - 1 - x % pixels_per_byte) * bits)
        if len(aligned) + len(row_data) > GROUP_LIMIT:
            flush()
        rewritten.append([width, height, advance, left, top, length, packed_offset])
        aligned.extend(row_data)
        packed_offset += length
    flush()
    data = b''.join(chunks)
    # Verify every stored group's actual glyph pixels after decompression.
    for compressed_offset, compressed_size, _, count, start in groups:
        raw = zlib.decompress(data[compressed_offset:compressed_offset + compressed_size], -15)
        cursor = 0
        for glyph in glyphs[start:start + count]:
            width, height, _, _, _, length, offset = glyph
            stride = (width + pixels_per_byte - 1) // pixels_per_byte
            packed = bitmap[offset:offset + length]
            for y in range(height):
                for x in range(width):
                    index = y * width + x
                    expected = (packed[index // pixels_per_byte] >> ((pixels_per_byte - 1 - index % pixels_per_byte) * bits)) & mask
                    actual = (raw[cursor + y * stride + x // pixels_per_byte] >> ((pixels_per_byte - 1 - x % pixels_per_byte) * bits)) & mask
                    assert actual == expected
            cursor += stride * height
        assert cursor == len(raw)
    byte_lines = ['    ' + ', '.join(f'0x{x:02X}' for x in data[i:i + 16]) + ',' for i in range(0, len(data), 16)]
    glyph_lines = ['    { ' + ', '.join(map(str, row)) + ' },' for row in rewritten]
    # Replace complete arrays, keeping intervals, kerning, metrics and names unchanged.
    replacement_bitmap = f'static const uint8_t {name}Bitmaps[{len(data)}] = {{\n' + '\n'.join(byte_lines) + '\n};'
    replacement_glyph = f'static const EpdGlyph {name}Glyphs[] = {{\n' + '\n'.join(glyph_lines) + '\n};'
    text = text[:glyph_match.start()] + replacement_glyph + text[glyph_match.end():]
    text = text[:bitmap_match.start()] + replacement_bitmap + text[bitmap_match.end():]
    table = f'static const EpdFontGroup {name}Groups[] = {{\n' + '\n'.join('    { ' + ', '.join(map(str, row)) + ' },' for row in groups) + '\n};\n\n'
    marker = f'static const EpdFontData {name} = {{'
    text = text.replace(marker, table + marker, 1)
    start = text.index(marker)
    tail, count = re.subn(r'    (true|false),\n    nullptr,\n    0,', lambda match: f'    {match[1]},\n    {name}Groups,\n    {len(groups)},', text[start:], count=1)
    assert count == 1
    text = text[:start] + tail
    text = '// Generated by scripts/compress_plus_builtin_fonts.py. Do not edit.\n' + text
    if not output.exists() or output.read_text() != text:
        output.write_text(text)
    return {'compressor': 'zopfli-0.2.3.post1-5-iterations-or-zlib9', 'name': name, 'coverage_bits': bits, 'glyphs': len(glyphs), 'groups': len(groups), 'raw_bitmap_bytes': len(bitmap), 'compressed_bitmap_bytes': len(data), 'group_table_bytes': len(groups) * 20, 'max_group_bytes': max(g[2] for g in groups), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'all_glyph_pixels_verified': True}


if __name__ == '__main__':
    results = [build(name) for name in ('notosans_cjk_12_rickyos', 'notosans_cjk_14', 'notosans_cjk_16', 'ricky_caption_10_regular', 'ricky_display_23_bold', 'ricky_headline_14_medium', 'ricky_title_16_bold', 'ricky_book_12_medium')]
    (PROJECT / 'generated/fonts/packing-report.json').write_text(json.dumps(results, indent=2) + '\n')
    for result in results:
        if result['compressor'] == 'upstream':
            print(result['name'], 'already packed upstream; copied unchanged')
            continue
        print(result['name'], 'saved', result['raw_bitmap_bytes'] - result['compressed_bitmap_bytes'] - result['group_table_bytes'], 'bytes; every glyph pixel verified')
