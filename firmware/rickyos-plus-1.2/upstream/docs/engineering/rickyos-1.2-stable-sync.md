# RickyOS 1.2: stable feature synchronization

Date: 2026-10-11 (Asia/Shanghai).
Target: private `chinoryunqin/RickyOS-dev`, `codex/rickyos-1.2-design`.
Visual baseline: `185bfe78004092174dcbb4e668e3697c4dee7e88`.

## Sources and scope

- Merge `rickyos-1.2` at `f201efa76ad5a2cc5c78f85883cf8204d16d0d24`.
  This includes `c23d33c3` and `f201efa7`, the 1.1.4-equivalent fixes for
  stale PMU key events, short power-key taps and battery-sample wakes.
- Port the functional changes from stable 1.1.5 commit
  `7b951473b1e3c294868018ba6effba500f14dbef`, released at
  `addc54a637da63d8f094de6a30cbdf1118c70d82`.
  Markdown gains common block/inline formatting, bounded tables, six-level
  ATX heading contents, source-offset migration and correct partial-section
  offset navigation. Back-to-Standby's switch controls both entry and exit;
  the independent side power key remains available.

This is not a blanket merge of the stable branch. Keep 1.2's product-only
architecture, removed XTC/OPDS/plugin code, direct grayscale, design resources,
main/interior layouts, reader menu and chrome. Leave `platformio.ini`, the
installer release catalog and official release artifacts unchanged. This sync
does not designate or publish a 1.2 release.

## Integration adaptations

1. Apply the middle-key policy without restoring the retired stock UI branch.
   Test its actual production branch with disabled/enabled/disabled transitions.
2. Guard hardware-only key bookkeeping in `onEnter()` and the main loop with
   `CROSSPOINT_EMULATED == 0`, matching the stable simulator compatibility fix.
3. Keep the 1.2 partial-reader guard and render path, adding the requested
   visible-offset lookup ahead of anchor/page checks rather than replacing UI.
4. Update the existing product-only TXT conversion test to expect Markdown
   cache version 5 and paragraph markup. Keep plain TXT expectations intact.
5. Markdown reuses the converter's two checked 8 KiB buffers: split the input
   buffer for reads, lines and table lookahead. No whole-document or additional
   full-screen allocation is introduced. Source maps and chapters remain on SD.

## Validation

- Focused CMake suite: 77/77 passed, including Markdown conversion, table bounds,
  chapter indexing, progress maps and library recognition after uploads.
- Native `simulator_rickyos` build passed.
- Native input QA: middle-key Off/On/Off and independent side power passed.
- Native Markdown QA: open, page turn, MD2 checkpoint upgrade and zero-offset
  upgrade passed; no reset to the first page during migration.
- Native table/contents QA: four tables, 29 heading labels/levels/anchors,
  actual row 10 resolving to page 9, all 31 pages through the end panel,
  reopening and line-spacing reflow passed.
- Read Pico hardware build passed: application 5,920,000 bytes, static RAM
  85,704 bytes. 4 KiB-aligned application size is 5,922,816 bytes, leaving
  630,784 bytes in the unchanged 6,553,600-byte slot (above the 512 KiB floor).
  SHA-256: `71411b1a19e91a79ca9179ad0913a4d1cd8c9c5974256a697feeb85b9a8f2b91`.
  The inherited development version remains `1.1.4-dev`; the branch name and
  this record, not a release designation, identify the 1.2 experiment.
- Full Python suite: 341 total, 327 passed, 13 skipped, one pre-existing
  clock-digit regeneration check failed (153.120 seconds). An independent archive of the
  untouched `185bfe78` baseline reproduces the clock-digit regeneration mismatch
  (`d1` advance 51 in the resource versus 52 with current Pillow/FreeType).
  This sync does not change the font, generator or corresponding test. Do not
  describe the full suite as green while that pre-existing check remains red.
- Formatting and `git diff --check` passed.

Native QA uses isolated temporary SD cards. Outputs containing sample text,
screenshots, logs and binaries stay under ignored `build/`; no user card or
USB device is read or written. Simulator results do not establish e-paper
latency, ghosting, battery behavior or physical input acceptance.

The SDK stays pinned to `96de1be6ce08eb732909e6e8149af8f892b9a2c5`.
Pre-existing local SDK changes remain local and are not reset or included in
this product sync. The tracked PMU compatibility script upgrades its narrow
SDK key declarations/implementation during a hardware build, as before.
