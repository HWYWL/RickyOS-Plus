#!/bin/bash
#
# Generates the RickyOS 12pt CJK header: the shared common set
# (cn_common_chars.txt) plus every GB2312 level-1 hanzi (gb2312_lv1.txt) and the
# common Traditional set: Big5 level 1 (big5_lv1.txt, A440-C67E) less its 300
# least frequent extra characters (big5_rickyos_chars.txt, from
# select_rickyos_big5.py), so author names such as 亨利 and Traditional titles
# such as 他們 render instead of tofu. Read Pico's high-density profile
# uses only the 12pt face, so the
# product swaps this one font and leaves the shared 8/10/12pt headers (and
# their common interval table) untouched for every other target.
#
# Same source, subset flags and fontconvert options as build-cn-builtin-fonts.sh;
# on the shared characters the glyph bitmaps are byte-identical.
#
#   PYTHON=/path/to/venv/bin/python bash build-rickyos-cjk-font.sh

set -euo pipefail

cd "$(dirname "$0")"

PYTHON="${PYTHON:-python3}"
SOURCE_OTF="../builtinFonts/source/NotoSansSC/NotoSansSC-Regular.otf"
SOURCE_SHA256="2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
CHARSET_FILE="cn_rickyos_chars.txt"
TMP_DIR="instanced_fonts/NotoSansSC"
SUBSET_OTF="$TMP_DIR/NotoSansSC-Regular.rickyos.otf"
FONT_NAME="notosans_cjk_12_rickyos"
OUTPUT_PATH="../builtinFonts/${FONT_NAME}.h"

if [ ! -f "$SOURCE_OTF" ]; then
  echo "Error: $SOURCE_OTF not found; see docs/engineering/chinese-build.md." >&2
  exit 1
fi
"$PYTHON" - "$SOURCE_OTF" "$SOURCE_SHA256" <<'EOF'
import hashlib, sys
digest = hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest()
if digest != sys.argv[2]:
    sys.exit(f'Error: source SHA-256 {digest} differs from the committed fonts\' {sys.argv[2]}')
EOF

# Common set first, then the GB2312 and Big5 level-1 hanzi it lacks, in file order.
"$PYTHON" - cn_common_chars.txt gb2312_lv1.txt big5_rickyos_chars.txt "$CHARSET_FILE" <<'EOF'
import sys
common = open(sys.argv[1], encoding='utf-8').read().strip()
seen = set(common)
counts = []
text = common
for source in sys.argv[2:4]:
    extra = [c for c in open(source, encoding='utf-8').read()
             if 0x4E00 <= ord(c) <= 0x9FFF and c not in seen and not seen.add(c)]
    counts.append(len(extra))
    text += ''.join(extra)
open(sys.argv[4], 'w', encoding='utf-8').write(text)
print(f'{sys.argv[4]}: {len(common)} common + {counts[0]} GB2312 + {counts[1]} Big5 level-1 characters')
EOF

mkdir -p "$TMP_DIR"
"$PYTHON" -m fontTools.subset "$SOURCE_OTF" \
  --output-file="$SUBSET_OTF" \
  --text-file="$CHARSET_FILE" \
  --unicodes="U+0020-007E,U+00A0-00FF,U+2010-2026,U+3000-303F,U+FF00-FF9F,U+FFA1-FFEF,U+FFFD" \
  --layout-features='*' \
  --notdef-outline \
  --recommended-glyphs \
  --no-hinting \
  --drop-tables+=DSIG,GSUB,GPOS

tmp_path="${OUTPUT_PATH}.tmp"
"$PYTHON" fontconvert.py "$FONT_NAME" 12 "$SUBSET_OTF" \
  --2bit \
  --additional-intervals 0x4E00,0x9FFF \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0xFF00,0xFFEF \
  > "$tmp_path"
if [ ! -s "$tmp_path" ]; then
  echo "Error: fontconvert.py produced empty $tmp_path" >&2
  rm -f "$tmp_path"
  exit 1
fi
# Frequency-ordered compressed groups (UI characters and common text in few groups);
# glyph order, metrics and intervals stay as fontconvert wrote them.
"$PYTHON" ../../../scripts/pack_rickyos_cjk_font.py "$tmp_path" --order cn_rickyos_group_order.txt
mv "$tmp_path" "$OUTPUT_PATH"
echo "  $(wc -c < "$OUTPUT_PATH") bytes ($(grep -E "Bitmaps\[" "$OUTPUT_PATH" | head -1))"
