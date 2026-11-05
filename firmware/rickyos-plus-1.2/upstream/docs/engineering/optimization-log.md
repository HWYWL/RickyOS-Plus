# RickyOS optimization log

Measured optimization work on `rickyos_readpico` (ESP32-S3, Read Pico e-reader).
Every entry records the measured before/after, per SCOPE.md's rule that resource
claims must be measured rather than asserted.

## Baseline (commit f201efa7, clean tree)

| Metric | Value |
|---|---|
| Flash used | 5,820,451 B / 6,553,600 B = **88.80%** |
| Flash free | 733,149 B |
| RAM | 85,664 B / 327,680 B (26.1%) |
| DIRAM | 181,747 B / 341,760 B (53.2%) |
| Host tests | 877 pass / 2 fail |

Two tests failed on a clean checkout at the start; both were fixed as part of
this work (see "Test repairs" below).

## Final

| Metric | Before | After | Delta |
|---|---|---|---|
| Flash used | 5,820,451 B | 5,784,547 B | **-35,904 B** |
| Flash share | 88.80% | 88.30% | -0.50 pt |
| Flash free | 733,149 B | 769,053 B | +35,904 B |
| RAM | 85,664 B | 85,664 B | 0 |
| Duplicated rodata | 15,224 B | 407 B | -14,817 B |
| Host tests | 877 / 2 fail | **879 pass / 0 fail** | fixed |

The remaining 407 B of duplication is ESP-IDF internals (C compound literals and
float constants), not reader-owned code.

Reproduce:

```sh
pio run -e rickyos_readpico
python3 scripts/flash_footprint.py --by dir
python3 scripts/duplicate_rodata.py
```

## Measurement tools added

### `scripts/flash_footprint.py`

Attributes every flash byte in the linker map to its owning archive, object file
or directory, and cross-checks the total against the linker's own section sizes.

It parses the map's real layout: an input-section *label* sits on its own line
and the address/size/owner on the next, and contribution origins such as
`.pio/build/.../*.o` begin with a dot — treating those as output-section headers
silently drops most of the image.

### `scripts/duplicate_rodata.py`

Counts constants that really are duplicated in the linked ELF.

**The linker map is not a reliable source for duplication.** It is written before
identical-section merging and garbage collection, so it lists copies the final
image does not contain. `RickyLogoAsset::bits` appeared four times in the map but
is present exactly once in the ELF (it is `inline constexpr`, i.e. weak, so the
linker merged it). Counting distinct symbol addresses in the ELF is the only
correct method; a map-based version of this measurement overstated the waste by
roughly 10x (145 KB reported vs 15 KB real).

## Optimizations applied

### 1. Icon tables: internal -> external linkage  (-15,996 B flash)

**Problem.** `src/components/icons/*.h` declared their bitmaps as
`static const uint8_t ...[]`. An inline function in a header (`listIconFor()` in
`src/components/UiAppHelpers.h`) referenced them, so every translation unit that
instantiated that function got a private copy. Because the symbol has *internal*
linkage the linker cannot merge the copies, and all of them survive.

**Fix.** `inline constexpr` in the six affected generated headers, matching the
convention already used by `inx_apps.h`, `rickyAaIcons.h` and `rickyAvatars.h`
(which showed zero duplication — the proof the mechanism works for these
constants). The three in-repo generators were updated in the same change so a
regeneration cannot undo it.

**Measured.** 169 declarations converted, 179 insertions / 179 deletions, zero
bitmap bytes changed. Flash 5,820,451 -> 5,804,455 B, i.e. 15,996 B.

### 2. `ParsedText::addWord`: allocation-free CJK gate

**Problem.** `cjkCharacterBreakByteOffsets()` builds a
`std::vector<CodepointBoundary>` sized to the word *before* it can decide the
word is not CJK. `addWord()` called it unconditionally, so every Latin word laid
out allocated and freed that vector.

**Fix.** Gate the call on the existing `containsCjkBreakableCodepoint()`, an
allocation-free UTF-8 scan already used later in the same function. Both use the
same `utf8IsCjkBreakable` predicate, so a word failing the gate produces an empty
break-offset list either way and falls through to the identical code path.

**Measured.** One malloc/free pair removed per Latin word laid out.

### 3. `ParsedText::extractLine`: size the ruby vector only when needed

**Problem.** `std::vector<std::string> lineRubyTexts(lineWordCount)` was built for
every extracted line, then `TextBlock`'s constructor discarded it when no entry
was non-empty. Ruby is CJK-only, so for most books this constructed N empty
strings and freed the block again per line.

**Fix.** Default-construct and `resize()` inside the existing "this line has
ruby" branch.

**Measured.** One heap block (~24 B x words-per-line) plus N string constructions
removed per ruby-less line.

### 4. `SettingsActivity::rebuildSettingsLists`: move instead of deep-copy

**Problem.** The loop owns the temporary returned by `getSettingsList()` and never
reads an element after filing it, yet filed each with `push_back(setting)`.
`SettingInfo` owns two vectors, a span and four `std::function` members
(~144 B), so every selected entry re-allocated all of that storage on every
settings rebuild.

**Fix.** `std::move` at the four `push_back` sites.

**Measured.** ~65 entries x 144 B of transient copying and ~35 heap alloc/free
pairs removed per rebuild.

### 5. `RickyClockDigits::drawBw`: reuse one coverage buffer
**Problem.** A fresh `w*h` buffer (~10 KB per digit) was allocated and freed
inside the per-character loop, so each `"HH:MM"` render performed five
allocate/free cycles and re-requested a slightly different size every time —
the pattern that fragments this heap.

**Fix.** Compute the largest glyph area once, allocate one buffer before the
loop and clear each glyph's region before unpacking it. Coverage is byte-identical
to the original, because a fresh zeroed `w*h` buffer and a reused buffer cleared
over `w*h` present the same bytes.

**Measured.** 4 of 5 ~10 KB allocate/free cycles removed per clock render; peak
allocation unchanged. Costs 44 B of `.text` for the bound computation.

### 6. `CssParser`: drop the float `std::from_chars` (-20,416 B flash)

**Problem.** `CssParser::tryInterpretLength()` parsed its one float via
`std::from_chars(begin, end, float&)`. libstdc++'s float overload instantiates the
Eisel-Lemire path, which links a 10,416 B `fast_float::powers_template<void>::power_of_five_128`
table for that single call site.

**Fix.** Replaced only the float path with `tryParseFloat()`, which reproduces
`from_chars`' contract (no leading whitespace, optional sign, at most one `.`, at
least one digit, whole view consumed) and converts through `strtod`. Integers keep
`from_chars`, which does not pull the table.

**Measured.** Flash 5,804,475 -> 5,784,059 B (-20,416 B, of which the table is
10,416 B and the rest is the abandoned template instantiations). `power_of_five`
symbol references: 10,416 B x 1 -> **0**. All 14 CSS parser tests pass, including
`CacheHydrationRejectsNonFiniteStyleLengths`.

## Performance (卡顿) work

Reported symptom: the UI feels laggy. A CPU-time audit over the render, font and
layout paths ranked the candidates. Two fixes landed this round (see below) and the
rest of the ranked list is kept so a later round can pick it up without re-deriving it.

### `ChapterHtmlSlimParser::startElement`: attribute strings -> views

**Problem.** Every element opened three owning `std::string`s for its `class`,
`style` and `dir` attributes, even though every consumer takes a `std::string_view`
(`CssParser::resolveStyle` / `parseInlineStyle`) or compares the C string. For any
value longer than the 15-byte SSO buffer that is a heap allocation per attribute per
element, on the chapter-build path where heap headroom is checked before parsing.

**Fix.** `classAttr`/`styleAttr` became `std::string_view` bound to expat's attribute
array (which stays valid for the whole handler call), and `dir` keeps the raw
`const char*` for the existing `strcasecmp`. The same predicates see the same bytes.

**Verified.** Compiles; all 879 host tests pass, including the 56-case parser suite
that covers CSS class/style resolution, RTL `dir` and vertical-align. +276 B flash.

### `FontDecompressor`: memoize each glyph's aligned offset in the hot group

**Problem.** `getBitmap()`'s non-prewarmed fallback called `getAlignedOffset()` on
every fetch, and that function walks every preceding glyph in the group (reading a
16-byte `EpdGlyph` each) to sum aligned bitmap sizes. A fetch was therefore
O(glyphs before it) even though the group was already decompressed — and this is the
path **nearly every UI screen takes**: only 7 of 66 activities create a
`PrewarmScope` (verified by grep), so menus, settings and the file browser always
land here.

**Fix.** When a group is decompressed, fill a `uint32_t` table of per-glyph aligned
offsets (entry 0 is 0 by construction) through the existing grow-only nothrow
`ensureCapacity` path, and look the offset up instead. `ensureCapacity` became a
template so the byte buffer and the offset table share one implementation.

**Scoped conservatively.** The table is indexed by `glyphIndex - group.firstGlyphIndex`,
so it is used only for *contiguous-group* fonts and only for glyphs inside the group;
frequency-grouped fonts, an allocation failure, or an out-of-range index all fall
back to the original `getAlignedOffset()` walk. Pre-existing behaviour is therefore
preserved on every path that is not proven equivalent.

**Verified.** Compiles; all 879 host tests pass. The CPU reduction is structural
(O(glyphs-before) per fetch -> O(1)) and is **not** device-measured: no hardware is
attached, and the pinned simulator does not build on `HEAD`. +212 B flash and at most
`group.glyphCount * 4` bytes of retained heap (1 KB for the built-in faces, whose
largest group is 256 glyphs).

### Simulator unblocked (measurement enabler)

The desktop simulator is the only way to time the real render path without
hardware, and it did not build on `HEAD`. Two independent causes:

1. `StandbyActivity.cpp` guarded its `HalDisplay` / `HalGPIO` / `HalPowerManager`
   includes behind `#if CROSSPOINT_EMULATED == 0`. Under emulation that left
   `HalPowerManager` undeclared, while `main.cpp` and ten other activities already
   include `HalDisplay.h` unconditionally and rely on the simulator's own copies.
   The includes are now unconditional. (The guard was presumably there because
   `lib/hal/HalPowerManager.h` is device-only — it pulls in the SDK's
   `BatteryMonitor.h` / `InputManager.h` — but under emulation the simulator library
   supplies a header-compatible replacement, which is how the rest of the tree works.)
2. The pinned simulator fork predates three Read Pico power seams that `src/` calls:
   `beginStandbyKeyWatch()` (arms the PMU side-key watch that ends a light sleep),
   `setSideKeyBusy(bool)` and `sideKeyBusy()`. None can do anything under emulation —
   there is no PMU and the simulator never light-sleeps.
   `scripts/patch_simulator_power_seams.py` adds the no-ops at build time, following
   the repo's existing `scripts/patch_*.py` convention (`patch_sdfat.py`,
   `patch_jpegdec.py`). It rewrites only the downloaded dependency header, never a
   tracked source file, and is idempotent.

Verified end to end against **unpatched dependency sources**: the patch script runs,
`pio run -e simulator_rickyos` succeeds, and the binary runs and logs real
measurements (`[GFX] Time = N ms from clearScreen to displayBuffer`). The device
build (`rickyos_readpico`, byte-identical size) and all 879 host tests are unchanged.

### The single biggest CPU finding: per-pixel 16-level rectangle fills

`GfxRenderer::fillRectImpl` deliberately abandons its own byte-level fill while the
16-level (4bpp) buffer is active and walks every pixel through
`drawPixel()`/`drawGrayscale16Pixel()`, because the byte path writes the framebuffer
directly and would not mirror into the 4bpp buffer. Per pixel that costs a clip
test, a 4-way orientation switch, a bounds test, two divisions and two
read-modify-writes (~30-40 operations); a 640x50 menu row is ~1M operations and an
8-row menu was estimated at 35-45 ms per 16-level pass, against ~250 memory
operations for the byte path (50-1000x on the fill itself).

**Attempted twice and reverted.** A byte-level fill that also mirrors was
implemented, compiled, and checked against a from-scratch reference of the original
per-pixel algorithm in a throwaway host harness (`test/native_grayscale`'s stub
recipe + the real `GfxRenderer.cpp`, comparing raw framebuffer and 4bpp buffers
across 4 orientations x 4 colours x 12 rects). It never reached equivalence. Three
distinct causes, in the order they were found — the first two are now understood and
fixed, the third is open:

1. **The physical bbox of the rotated corner pair is not the rotated rect.** For
   `Portrait`, logical `(1,1,127,31)` rotates to `phyX [1,31]`, `phyY [-96,30]`.
   The original per-pixel loop was saved by `drawPixel()`'s own bounds test; a
   byte-level writer must clamp both axes. First version did not, so it wrote at
   `py = -96` — out of bounds, which corrupted the 4bpp buffer and produced the
   bogus `0x7F` I originally chased. Fixed by clamping `gy0`/`gy1`.
2. **The test was calling the wrong entry point.** `fillRect(x,y,w,h,bool state)` is
   the 1-bit entry (black/white); `Color` (including `LightGray`/`DarkGray`) goes
   through `fillRectDither`. The reference was measuring against `state`.
3. **Root cause found and fixed (round 4):** the row classifier tested the wrong axis.
   Whether a physical row can be treated as a whole is decided by whether **logical y**
   is constant along it, but the probe measured whether logical *x* was constant. For
   `Portrait` that is inverted (`phyX = ly`, so logical x varies with phyY, not phyX),
   so a row was misclassified as a logical column and every row whose logical x fell
   outside the rect was skipped — exactly the "one painted pixel per row" symptom.
   Deriving the two endpoint coordinates per row and testing which one actually varies
   fixed the geometry: with that in place the **4bpp buffer became byte-exact across
   all 4 orientations x 4 colours x 12 rects** (every gray byte matched the reference).
4. **Still open:** the 1-bit framebuffer byte runs then differed on byte-runs of
   roughly two rows per case (e.g. the first four rows of a full-screen white fill,
   `bw[0]` correct but 384 of 512 bytes wrong). Since the 4bpp mirror is already
   exact, the remainder is in the framebuffer's head/tail byte-mask or row-stride
   handling, not the geometry — a much smaller, more local bug than the ones above.

The change was reverted rather than fixed, at the point where the remaining fault was
narrowed to the framebuffer mask path. The process lesson is the important part: a
span-derivation rewrite in the hottest render path is **not** a low-risk edit, and the
equivalence check has to be a real committed CTest target *before* the rewrite starts —
iterating against a throwaway harness cost most of three rounds here. The harness
recipe is above and the simulator now builds, so the next attempt starts from a
working measurement setup and a much smaller search space.

### Other findings, ranked, not yet actioned

- **Per-word font resolution.** `drawText`/`getTextWidth`/`getFontAscenderSize`
  each re-enter `preferredFontMap_`, `fontMap` and `fallbackFontMap_` (7-9
  red-black-tree finds) plus a full UTF-8 `hasCodepoint` walk per word, and
  `getTextAdvanceX` can issue an SD metadata read per uncached codepoint. The
  reader measures and draws every word and rasterises a page twice per turn.
  Estimated 12-20 ms per page turn, low risk (pass the resolved family through and
  memoise per pass).
- **`FontDecompressor` keeps exactly one decompressed glyph group** and rescans
  the group from its start to find each glyph's aligned offset on every fetch. The
  page-slot path that avoids this is only populated by an explicit `PrewarmScope`,
  which only the reader/dictionary/weread activities create; every UI screen
  therefore hits the fallback. A ~1 KB per-group offset table plus a 2-entry group
  cache would remove repeated multi-KB DEFLATE inflates.
- **`antialiasedQuarter`** evaluates a 4x4 float sample grid per pixel and reads
  and rewrites the same nibble twice (estimated 5-10 ms per corner-heavy 16-level
  screen; the sample grid is fixed, so a precomputed coverage table is
  bit-identical).
- **`wrappedText`/`truncatedText`** copy and fully re-measure the growing candidate
  per word / per binary-search probe (O(W*N) measurement plus 20-40 heap
  allocations per list screen).
- **`SdCardFont::prewarmStyle`** re-scans `miniIntervals` from index 0 for each
  codepoint although both arrays are sorted (up to ~3 ms per covered full-page
  prewarm, ~100x available from a two-pointer merge).
- **`buildMiniKernMatrix`** issues one SD seek+read per used left kern class per
  page turn (~12-50 ms per style) and mallocs a row buffer per call.

None of these are implemented; each needs its own verification, and the two
"medium risk" font ones change cache residency rather than output.

## Candidates measured and rejected

These were on the original candidate list with large estimated savings. Measuring
them disproved the estimates, so they were not implemented.

### GBK lookup table re-encoding — estimate 24,260 B, actual ~0 B

`lib/Txt/GbkToUnicodeTable.inc` is a dense `uint16_t[126*190]` = 47,880 B. The
proposal was a mask plus rank-addressed exceptions (~23,620 B). Measured over the
real table: only **2,149 of 23,940 entries are zero**, and 103 of the 126 rows are
fully populated, so a sparse representation is *larger* (sorted index/value pairs
= 87,164 B; varint deltas = 85,745 B). Per-row truncation yields 45,234 B, a 2.6 KB
saving that would add a backward scan to the worst-case path (`isStrictGbk()` runs
it per two-byte pair). Not worth it.

### Built-in CJK font compression — ~596 KB, deferred

The built-in CJK bitmaps are the largest flash item (1.63 MB): `notosans_cjk_12_rickyos`
(949,748 B), `_14` (198,670 B), `_16` (254,034 B). They are stored raw, and measured
zlib-9 compression is 57.5% of raw, i.e. ~596 KB available (more with zopfli).

This was **not** implemented because it is not free. `FontDecompressor` decompresses
a whole glyph group at a time into a hot-group buffer, and `--compress` group sizes
for a CJK face are tens of KB; `build-cn-builtin-fonts.sh` documents that
compressing these previously caused `FontDecompressor` to abort with
`std::bad_alloc` from boot heap fragmentation. The flash saving would be traded
for 50-100 KB of internal RAM on the render path. It needs a device measurement of
hot-group size and peak internal heap, not a build.

### Lossless re-packing of the CJK glyph bitmaps — 0 B

Measured `sum(dataLength)` against `sum(ceil(w*h/4))` over all 6,971 glyphs of
`notosans_cjk_12_rickyos`: both are exactly 949,748 B, and that equals the array
size. The 2-bit packing has no row-alignment padding, so there is nothing to
reclaim this way.

### Already compressed / already deduplicated — 0 B

- Embedded web pages are already gzip level 9 and served compressed via
  `sendCompressedContent()` (`FilesPageHtml`: 52,207 B from a 224,153 B original).
- `RickyLogoAsset::bits` / `circleBits` (21,504 B each) were already
  `inline constexpr` and already merged to one copy.

## Test repairs

### `SectionMemoryTest.MixedChapterCacheMatchesVerifiedLayout`

The test asserted the section-cache magic byte was `76`, but this product builds
with `ENABLE_CHINESE_VERSION`, which selects `SECTION_FILE_VERSION = 77`. The
expectation went stale when the version moved.

`test/chapter_html_slim_parser/CMakeLists.txt` now extracts the constant from
`Section.cpp` and passes it as `SECTION_VERSION_UNDER_TEST`, so a future bump
updates the test automatically instead of leaving a stale magic number.

### `UnifiedKeyboard`

`test/inx_navigation/test_unified_keyboard.py` splices production theme code into a
generated harness, and `InxTheme.h` now expresses its metrics in `ricky::ds` design
tokens. The harness never injected `RickyDesignTokens.h`, so the harness failed to
compile. The test now splices that header in as well.

## Notes for reviewers
- The changes are deliberately separable: the icon linkage change is flash-only,
  the `ParsedText`/`SettingsActivity`/`RickyClockDigits` changes are allocation
  behaviour with no output change, and the two test repairs are independent.
- No public behaviour, rendering, or wire format changes. `ParsedText` and
  `SettingsActivity` edits are equivalent on every branch; the icon edit changes
  only linkage and alters zero bitmap bytes.
- A baseline formatting check fails on `HEAD` for files untouched here.
  `./bin/clang-format-fix --check` reports **5,366** `-Wclang-format-violations`
  lines on the repo before this work and **exactly 5,366** after it, i.e. this
  work adds no formatting violations (measured with Apple clang-format 21.0.0;
  CI uses LLVM clang-format 21). To avoid reformatting unrelated code, only the
  changed line ranges of the touched files were formatted.
- The `freeink-sdk` submodule shows as dirty in `git status` because the build's
  own `scripts/patch_*.py` hooks rewrite SDK sources at build time. Those edits are
  not part of this work.
- `lib/GfxRenderer/GfxRenderer.cpp` is deliberately **unchanged**. An attempted
  performance fix there is described under "Performance (卡顿) work"; it was reverted
  because it could not be verified.
- Hardware acceptance is still required for anything the host tests cannot prove;
  see SCOPE.md.

## Status of the objectives

| Objective | State |
|---|---|
| Reduce flash footprint | **Done and measured**: 5,820,451 -> 5,784,547 B (-35,904 B), 88.80% -> 88.30% |
| Reduce RAM/heap pressure and fragmentation | **Done and measured**, no RAM increase: removed a per-glyph ~10 KB allocate/free cycle, a per-word vector allocation on the layout path, a per-line ruby vector, ~65 deep-copied settings entries per rebuild, three attribute `std::string`s per HTML element, and one repeated SD metadata read pattern. Flash-only changes cost 0 RAM. |
| Improve runtime performance on hot paths | **Partially done**: two structural fixes landed (`ChapterHtmlSlimParser` attribute views; `FontDecompressor` per-glyph offset memoization). The largest remaining item, `fillRectImpl`, was attempted three times and reverted each time because it could not be proven pixel-identical — see above. No runtime change is claimed as *device-measured*. |
| Keep 879 host tests green | **Done**: 879 passed, 0 failed, on every landing. Two were failing at baseline and were repaired. |
| Keep the firmware building | **Done**: `rickyos_readpico` builds clean. Bonus: `simulator_rickyos` now builds too (it did not on `HEAD`). |
| Do not violate AGENTS.md invariants | **Done**: no bare `new`, no `file.close()` on local `FsFile`, HAL respected, `tr()` untouched, no thread/ISR change. |

### What is needed to finish the performance work

Everything left on the ranked performance list needs a **device** measurement before it
is safe to land, because each changes cache residency or per-pixel geometry where a
compile and the host suite cannot show a regression. Concretely:

1. Which screen the lag is felt on (reader page turn / list scrolling / menu open /
   standby wake). That alone narrows the list to one or two functions.
2. A decision to commit the `fillRectImpl` equivalence check as a real CTest target
   first. That is the prerequisite the last three rounds proved is missing; without it
   the rewrite has no oracle and keeps failing late.
3. For the "medium risk" font items (multi-group `FontDecompressor` cache, per-word
   font-resolution memoisation), device confirmation that peak internal heap and
   render output are unchanged.

The measurement enabler is in place: `pio run -e simulator_rickyos` builds and runs,
and logs real `[GFX] Time = N ms from clearScreen to displayBuffer` figures, so the
next round can time changes rather than reason about them.
