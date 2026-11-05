# RickyOS navigation icons

Original hand-authored SVG artwork for this product, not renamed stock icons.
The home, open book/bookmark, soft tile grid, control panel, and progress bars
share a 56-unit canvas, 3.6-unit rounded strokes, black ink and generous margins.
They echo the friendly simplified logo without shrinking a portrait into a tab.

The five destinations and their order are unchanged. `RICKYOS_PRODUCT` alone
selects these resources; other products keep the upstream INX icons. Top/bottom
placement, selection marker, safe area, gestures and hit-testing are unchanged.
Use the existing theme renderer; do not add an SVG decoder or framebuffer.

Regenerate the checked-in resource (developer-only Node.js + sharp 0.35.4;
the tested bundled sharp uses librsvg 2.62.91):

```sh
node scripts/build_rickyos_navigation_icons.cjs
node scripts/build_rickyos_navigation_icons.cjs --out /tmp/ricky-icons.h --preview /tmp/ricky-icons.png
python -m unittest discover -s scripts/tests -p test_rickyos_navigation_icons.py
```

Set `NODE_PATH` when sharp is in a separate developer runtime. Firmware builds
do not need it. Both native 38px/56px resources use MSB-first packed bytes,
1=white/transparent, 0=ink, with white padding; the renderer does not resize them.
The total payload for both resolutions is 2910 bytes of constant storage;
only the selected resolution is referenced. There is no additional heap use.

Verify on Read Pico: check all five destinations, taps in the gaps, selection
indicator, portrait/landscape and both tab placements. Native simulator
screenshots demonstrate layout, not physical e-ink legibility/ghosting.
