# RickyOS device Pinyin: research and first prototype

Date: 2026-10-12. Local private branch `codex/rickyos-1.2-design`.
No commit, push or public release is part of this work. On 2026-10-12 the
private Read Pico build was flashed to the identified test device as described
below; that is not a 1.2 release.

## Evidence and reference boundaries

* [厂长 / read-pico-reader](https://github.com/wegooo-cell/read-pico-reader/tree/bb50077d51eb266e1add0cf050e79c2ac7ba95c6):
  examined `main/ui/ui_ime.{c,h}`, `ui_ime_phrases.h`, `ui_keyboard.c`, and
  `tools/generate_ime_dictionary.py`. Continuous full Pinyin, T9, bounded DP
  composition, previous-text phrase association, syllable preference, and
  paged homophones are present. Raw input is bounded to 48 bytes; a candidate
  batch contains 24 items, each with a 96-byte UTF-8 text and consumption length.
  Both the candidate result and decoder workspace explicitly require PSRAM;
  the workspace caches syllable lookups and is released on keyboard exit.
  Keyboard code keeps composition separate, checks insertion before consuming
  raw input, and owns refresh-area state. These are source observations, not
  measured optical panel latency or a quality benchmark of its latest release.
* [kiikoread official site](https://kiikoread.com/) currently links that same
  source repository, official release path and GitHub installation site.
  This is not independent evidence for a second decoder implementation.
* [WhiteOS public source snapshot](https://github.com/446599/whiteos/tree/56df0d1c24788e284a7c43d0bd0afe2ed1a55048)
  includes a website, release data and third-party source notices. This audit
  did not locate its actual `ui_ime`/keyboard implementation in the published
  tree. Do not infer its decoder quality or architecture from screenshots.
* [LVGL v9.2.2 Pinyin documentation](https://github.com/lvgl/lvgl/blob/v9.2.2/docs/others/ime_pinyin.rst):
  26-key and 9-key modes; configurable syllable-to-character dictionary.
  Its default dictionary is explicitly tailored to a roughly 1,000-character
  font; the documentation recommends a custom font and dictionary. Adding LVGL
  solely for its IME would duplicate our existing FreeInkUI keyboard stack.

No competitor UI assets or decoder code were copied into RickyOS. The small
phrase list and decoder are original product code. The independently generated
character readings come from MIT-licensed pypinyin v0.55.0; source SHA-256 and
the complete notice are retained in the generator and `data/ime/LICENSE.pypinyin.txt`.

## Implemented first round

`src/util/PinyinInput.{h,cpp}` is a HAL/UI-independent decoder:

* Full Pinyin, case normalization and `v` for ü, including tone-bearing source
  forms such as `lǜ`; explicit apostrophes preserve `xi'an` boundaries.
* A compact, flash-resident dictionary: 409 syllables, 4,572 distinct Han
  characters, 6,388 readings. Its 16,048-byte table uses 30-bit syllable codes
  instead of padded seven-byte strings. A binary search replaces string-key
  scans. Its characters are the intersection of GB2312 and the **shipped**
  product body font, not a promise of complete GB2312 support.
* Original small phrase list plus bounded DP composition; single-syllable
  prefix candidates remain available if a complete sentence cannot be formed.
  Selection consumes exactly the candidate's raw prefix; the suffix is not lost.
* Five candidates per page, forward/back pages without silently truncating
  homophones. Exact words precede synthesized sentences.
* Sixteen recent selections influence exact-raw ranking **within that keyboard
  session only**. No account, telemetry or per-key SD write; not persistent learning.
* Fixed session storage: 1,162 bytes in the host build, including DP,
  candidates and preferences. No heap/PSRAM allocation, disk read or network
  request inside the decoder's per-key path. The activity still has its existing
  fallible allocation, committed `std::string`, font caches and display buffers;
  this is not a claim that the whole keyboard is allocation-free.

`KeyboardEntryActivity` retains the SDK keyboard, shared hit geometry and
TouchHoldRouter. Normal text fields gain Chinese/English mode, a candidate row,
composition strip and explicit `abc` literal submission. Password and URL fields
retain their original keyboard behavior. Selecting a candidate and finishing the
activity always require distinct gestures. Back first clears pending composition.
Insertion blocked by the field's byte limit does not consume its Pinyin.
Generation-coded candidate ids reject releases against old candidate tables.
The product path serializes activity mutation with the existing render lock.

Product text-entry labels use the same regular BODY family and direct-gray
manager policy as the rest of the experimental UI. Empty candidates have no
outline, disabled controls use gray, and transient touch-down highlighting does
not request an extra panel frame. This mechanism reduces requested refreshes,
but **no device input latency/ghosting measurement has been made**.

## Verification and limits

Run:

```sh
python3 scripts/tests/test_pinyin_input.py
python3 test/inx_navigation/test_unified_keyboard.py
pio run -e simulator_rickyos
pio run -e rickyos_readpico
python3 tools/rickyos-browser/check_pinyin.py
```

The decoder tests use actual production code with ASan/UBSan: overflow and
invalid input, upper/lower case, ü, whole words, continuous composition,
unconsumed suffixes, apostrophes, learning/reset, backspace, every dictionary
syllable and candidate page, and shipped glyph coverage. Host throughput numbers
are not ESP32 or panel measurements. A fourth test compiles the production
`insertUtf8`/`commitIme` methods with only publication collaborators stubbed:
a five-byte field cannot consume the six-byte word `你好`; extending it to six
bytes commits at a UTF-8 boundary and clears composition. Existing keyboard regression checks cover
3,840 scenes for each density profile; their harness now includes the existing
product design-token header rather than failing on its undeclared namespace.

The isolated real-simulator test enters the profile editor, taps `nihao`,
selects `你好`, separately confirms, checks the saved settings JSON, then reopens
and verifies Back clears composition before cancelling. It also switches to
English, saves a Latin suffix, reopens in Chinese, and verifies explicit `abc`
commits raw Latin instead of the first Han candidate. It does not modify the
user's simulator card or attach to USB.

This is **not yet a competing-quality complete IME**: the phrase list is small,
there is no statistical language model, persistent personalized dictionary,
fuzzy-spelling correction, shorthand, nine-key UI or punctuation association.
An unsupported rare character still needs a future extended-font/dictionary
solution. Do not claim “surpasses other firmware” without comparable task tests.

The existing 6.25 MiB app partition is not changed. Re-evaluate actual binary
size and erase-aligned headroom for every candidate. The initial prototype:

* ELF Flash report: 6,426,867 bytes; static RAM: 85,808 bytes.
* Actual app image: **6,432,128 bytes**; erase-aligned size: **6,434,816 bytes**.
* Remaining app-slot headroom: **118,784 bytes (116 KiB)**. It fits the partition
  but fails the existing 128 KiB experiment guard and 512 KiB public release guard.
* SHA-256: `3236f0008612aea1d91af8b06c62bbb373770f49474be5d7fdae44acf432db73`.
* The inherited embedded version remains `1.1.4-dev`; the private branch label
  is **not** a released 1.2 version. No release metadata was bumped in this task.

A successful `pio run` alone is not permission to flash/publish. This first
artifact was not flashed. The 30-bit key packing saved 1,636 table bytes
without deleting characters.

For the hardware trial, `rickyos_readpico` omits its private timing-diagnostic
strings and verbose DEBUG-level logs. The version, activity entry and completed
reader-page markers remain at INFO level for boot acceptance. No Pinyin
dictionary entries or system apps were removed. The rebuilt firmware image is
**6,388,480 bytes**, erase-aligned to **6,389,760 bytes**, leaving **163,840
bytes (160 KiB)** of app-slot headroom. This clears the existing 128 KiB trial
gate; it still does not clear the 512 KiB public-release gate. SHA-256:
`c9736beebea46ca7b42e47c4834a2babd1b996d7725996fc07aa6a7e88827e6c`.
The embedded version is still `1.1.4-dev`; do not market this as 1.2.

The local-only installer (kept outside this source repository as
`device-installations/flash-1.2-pinyin.py`) pinned that binary and targeted
only the verified Read Pico, MAC `14:c1:9f:d1:9f:58`, app slot `0x10000`.
The local installation record is
`device-installations/rickyos-1.2-pinyin-20261012-030135-438451/install.json`.
Before writing, the live 6.25 MiB app slot was backed up and its device digest
matched. After writing, the candidate digest matched twice; partition table
and OTA metadata were unchanged. A released-DTR/RTS reset produced version,
`EpubReader` entry and completed page-render logs without a fatal marker.
The other attached Metalio device and SD card were not touched. Hardware IME
typing/latency/ghosting still require visual and touch acceptance on the device.

Next acceptance priorities: representative nickname/home-note/search phrases,
correction and candidate hit accuracy on the four-inch screen, input-to-visible
latency at median/p95, repeated typing and deletion, long-press ownership,
reading-parent heap recovery, and cold/warm boots. Then expand a clearly licensed
frequency-ranked lexicon, bounded persistent learning, and T9 as separate rounds.
