# Read Pico 1.2 transparent wallpaper — local candidate

## Product behavior

Entry: **Apps → Screensaver → Standby → Style → Transparent wallpaper**.
Choose a PNG from the SD card (`/images` initially), preview it, then confirm
**Set transparent wallpaper**. The original PNG is copied to `/standby.png`;
the source is not changed. The ordinary image viewer's transparent PNG option
also enables this same standby face on RickyOS, not the legacy power-off-only
bitmap setting. Remove transparent image leaves the face in keep-page mode.

Standby retains the current visible page, including its sixteen-level gray,
centers the PNG using Complete image / Fill screen, without an added standby
label, icon or white status backing. It does not tick or synchronize the clock. A redraw does
not apply alpha twice. Wake returns to the activity underneath; its normal
full redraw removes the sticker. The middle-key preference and
physical power-button wake paths are unchanged.

Use a PNG with a transparent background; an opaque PNG covers the page. RGBA8,
gray+alpha8, indexed transparency and supported gray/RGB color-key PNGs use
real 0..15 coverage, not checkerboard transparency. Native scaling currently
uses nearest-neighbor, not bilinear; panel-size artwork (684 × 1216 portrait)
avoids scaling artifacts. Other decoder restrictions remain, including the
8 MP source-area and row-buffer limits. No BOOX artwork is bundled.

Scope/limits for this candidate:

- Native retained transactions target Read Pico. Unsupported targets, global
  inverted display mode and unknown panel baselines retain the existing B/W
  keep-page fallback without applying a PNG. Do not advertise inverted
  transparent wallpaper as supported yet.
- A missing or failed PNG decode keeps the original native page unchanged;
  partial decoded rows are cancelled before the retained transaction restarts.
- Power-off adds its distinct status without applying alpha again. Direct
  power-off from another page retains that page; it does not install a sticker.
- Simulator verifies pixels/lifecycle, not physical residual image, latency,
  light-sleep current, card-removal timing or power-loss safety.

## Display and memory contract

`GfxRenderer::beginRetainedGrayscale16()` uses a HAL capability seam. The Read
Pico adapter enters the existing facade transaction (synchronizing pending
display work), then restores the last successful `g_hl.back_fb` and `g_base`
B/W proxy into existing staging storage. The held-diagonal native profile is
selected only after a known baseline is restored. No additional screenshot,
whole-page heap allocation or persistent frame is introduced.

PNGdec and its expanded gray/alpha scanline scratch are allocated on demand
through the existing fallible decoder path and freed before display commit.
They cannot fit on a task stack; the ordinary-allocator preflight now uses
`HalMemory::getDefaultHeap()` so registered PSRAM is not ignored. This is a
heuristic, not a reservation. Compressed PSRAM input retains the existing
2 MiB cap and reserve. Native output never creates a four-level pixel cache.

Installation uses 1024 bytes of bounded stack copy scratch and `.part` / `.bak`
renames. Short read/write, rename and settings-save failures preserve/restore
the preceding image where possible. An interrupted between-renames install
recovers a complete backup on settings/face entry; `.part` is never displayed.
FAT renames/settings writes are not an atomic power-loss transaction. Removal
clears a stale backup first so it cannot resurrect an intentionally removed PNG.

The checked maintained adapters are `scripts/patch_rickyos_epdiy.py` and
`scripts/patch_rickyos_simulator.py`. Generated SDK/dependency edits are not the
source of truth; pre-existing unrelated SDK changes are preserved.

## Validation and handoff

- `scripts/tests/test_native_grayscale.py`: actual renderer + pinned production
  PNGdec, RGBA/gray-alpha/indexed/key transparency, four orientations, native
  baseline preservation, cancelled staging, low heap and allocation failures;
  existing BMP/JPEG/AirPage/sleep cases remain included.
- `test_native_gray_refresh_policy.py`: fresh SDK patch, repeated patch,
  native baseline/proxy restoration, cancel/retry and unknown-baseline refusal;
  existing real driver/highlevel/PMU transaction harness. GNU queue/allocator
  wrappers are Linux-only, not asserted by the macOS run.
- `test_rickyos_transparent_wallpaper.py`: production face/install control
  flow, fit/fill, decode rollback, one-time alpha application, copy/rename/save
  fault injection, interrupted-install recovery, checked simulator adapter.
- `tools/rickyos-browser/check_transparent_wallpaper.py`: isolated simulated
  card; compare home/reader before, asleep, repeated redraw and awake; assert
  all pixels outside the sticker are unchanged (including the footer); exercise picker, image
  preview, installation and persisted settings. Screenshots are ignored under
  `build/transparent-wallpaper/screenshots`.
- Product regression: 219 checks, 207 passed, 11 optional-dependency skips,
  one pre-existing clock-digit regeneration mismatch (Pillow/FreeType rounds
  the digit `1` advance to 52 instead of the committed 51). The font is unchanged;
  do not regenerate it as part of this feature.

Build both `rickyos_readpico` and `simulator_rickyos` before hardware testing.
The candidate stays local on `codex/rickyos-1.2-design`; no release or remote
push is implied. Keep actual binary size/SHA with the ignored candidate artifact.
The existing tight OTA slot still needs a release-budget review.

## No standby status chrome

After hardware feedback, the added standby caption is removed completely, not
replaced by another symbol. Both native retained rendering and the B/W fallback
omit it; the shared indicator also refuses non-power-off calls. Keep Page is
the original page alone; Transparent Wallpaper is the original page plus PNG.
No settings migration, new asset, heap allocation, timer or framebuffer is added.

Power-off keeps its distinct localized label in the caption Regular font,
`ink-2` and a borderless paper pill. Native power-off image rendering does not
copy the B/W proxy over it, which would discard its gray text and overwrite
the rounded corners. Check standby → power-off, missing/corrupt PNG, wake and
landscape on hardware. The simulator requires the complete footer to remain
pixel-identical during standby; these checks do not establish physical residual
image quality.

Hardware acceptance: choose both an indexed and RGBA PNG, verify Fit/Fill and
the original file, repeat ten sleep/wake cycles from a gray UI page and reader,
check all reader orientations, replace/remove the picture, disconnect SD during
copy, and repeat on battery long enough to exercise light sleep. Check standby
key disabled/enabled independently and report inverted mode as fallback until
a dedicated implementation is accepted. No hardware acceptance claimed yet.
