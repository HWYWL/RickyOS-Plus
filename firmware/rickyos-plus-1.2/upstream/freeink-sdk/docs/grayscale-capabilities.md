# Grayscale capabilities

Query `display.grayscaleCapabilities(mode)` before choosing a grayscale upload
path. The descriptor is returned by value and allocates no memory. Querying does
not select a waveform, prepare the panel, or change uploaded data.

```cpp
const auto gray = display.grayscaleCapabilities(freeink::GrayscaleMode::Overlay);
if (!gray.supported()) {
  // Render the page in B/W.
} else if (gray.stripUploads) {
  // The existing writeGrayscalePlaneStrip() API is available.
} else {
  // Use the existing complete-plane upload calls.
}
```

## Modes and encoding

`Overlay` describes the existing B/W-base plus LSB/MSB-mask pipeline used by
CrossPoint. The `encoding` is `OverlayMasks`: in (LSB, MSB) order, dark gray is
(1,1), light gray is (0,1), and B/W pixels are (0,0), with black versus white
supplied by the base frame. This is the host input encoding; a driver may convert
it internally before sending controller RAM.

`Absolute` supplies a complete four-tone image: black (0,0), dark (1,0),
light (0,1), white (1,1). Every pixel, including text and background, must be
present in both planes. The UC8279 X3 and the X4 SSD1677 configuration (including
SSD1677 X4 Pro/Classic), Sticky, and the shared Waveshare 3.97-inch configuration
advertise this mode by default. The UC8179 and UC8279 variants of the X4,
X4 Pro, and X4 Classic also accept absolute planes through this API. They use
full-plane uploads, retaining their controller-specific gate padding and polarity.
UC8279 selects its existing image-quality bank explicitly; UC8179 retains its
existing stock grayscale bank. This is an input-contract integration, not a new
UC8179 waveform or a promise of improved physical tone separation. Paper Mono
retains its existing path.

The X3 uploads the stock XTH4 rows to registers 20/24/22/23/21. The SSD1677
driver complements the common host planes for the native factory selectors;
it retains the factory LUT bytes and the C7 activation/power-down sequence.
SSD1677 absolute mode uses a single activation: the typed base call selects the
input mode without displaying a B/W intermediate image (`base = Combined`).
UltraChip modes retain their controller-specific B/W conditioning, so callers
still prepare the actual B/W image and call the same typed entry point.

```cpp
if (display.displayGrayscaleBase(freeink::GrayscaleMode::Absolute)) {
  display.copyGrayscaleBuffers(absoluteLsb, absoluteMsb);
  display.displayGrayBuffer();
}
```

The typed base call fixes the mode before any plane upload. It returns false
without painting a base if the mode is unavailable. Complete-plane calls or
consecutive full-width strips starting at row zero must cover both planes. Start
with LSB; strips may then interleave the two planes.
Missing, duplicate, out-of-order, null, and out-of-bounds absolute uploads are
never activated. `cleanupGrayscaleBuffers(nullptr)` cancels an incomplete pass;
normal painting, inversion changes, and sleep also cancel it. The next B/W
refresh is forced clean after an absolute pass or cancellation, even when a
caller has restored controller RAM in the meantime. This recovery belongs to
the driver, not to an application-side panel-state flag.

CrossPoint automatically selects this mode for unfiltered opaque sleep images,
EPUB sleep covers, and grayscale BMP viewing on supported panels. Gray text-AA,
EPUB/XTC page rendering and regular white-as-transparent BMP overlays keep their
existing overlay pipeline. Alpha BMP and PNG sleep overlays select absolute mode
on supported panels, retaining the composed B/W background in both planes and
rewriting every visible overlay pixel. No extra framebuffer is allocated. The image quantizer uses evenly spaced levels for
absolute covers and a separate `_original` BMP cache name, so older AA-tuned
cover caches are not silently reused. Decode/rewind failures cancel the absolute pass. SSD1677 keeps the previous
display visible; controllers with separate conditioning keep their B/W base. No user setting or build flag is
required.

An unsupported mode returns the default descriptor: `supported()` is false and
all upload/overlap flags are false. Do not silently fall back from Absolute to
Overlay with the same input bytes.

## Execution properties

| Field | Meaning |
|---|---|
| `base` | `Separate`: the base is activated separately. `Combined`: `displayGrayscaleBase()` retains it and the gray pass activates both together. |
| `stripUploads` | `writeGrayscalePlaneStrip()` accepts this mode's encoding. False means use complete-plane uploads, not that grayscale is unsupported. |
| `asyncBase` | An ordinary asynchronous B/W refresh can supply the base. Dedicated X3 grayscale-base waveforms do not qualify. |
| `stagingWhileBusy` | Strip uploads and `prepareGrayscaleTarget()` stage host data without SPI access while a B/W waveform is pending. This does not authorize arbitrary display calls while BUSY. |

The facade reports no grayscale while inverted or without a driver. A pending
inversion transition disables `asyncBase`. CrossPoint's renderer additionally
disables `asyncBase` when its fading fix is active. Re-query after changing
controller or inversion settings; the descriptor is a snapshot, not an object
to cache for the display's lifetime. Support describes the implemented path,
not a guarantee that later buffer allocations cannot fail.

## Compatibility and migration

Drivers implement one `grayscaleCapabilities(mode)` override. The default
`PanelDriver` implementation advertises no grayscale. Existing boolean methods
remain compatibility wrappers for Overlay mode:

- `supportsStripGrayscale()` -> `stripUploads`
- `supportsAsyncGrayscaleBase()` -> `asyncBase`
- `supportsBusyGrayscaleStaging()` -> `stagingWhileBusy`
- `combinesGrayscaleBase()` -> `base == GrayscaleBase::Combined`

Custom PanelDriver subclasses should move their old boolean overrides into the
new descriptor; the facade now reads that descriptor directly. The public
FreeInkDisplay/EInkDisplay compatibility calls remain available.

CrossPoint exposes the same descriptor through HalDisplay and GfxRenderer.
EPUB/XTC and shared reader helpers use that query. Existing upload, preparation,
activation, and cleanup calls are retained; this change consolidates capability
selection, not the entire refresh lifecycle. CrossPoint must build against an
SDK containing this header and query (the local development configuration points
to the sibling SDK checkout).

## Validation

The display host tests cover mode selection, exact selector polarity, full/strip
equivalence, incomplete uploads, LUT mapping, power state, recovery, inversion,
and wake, alongside the existing X3/Pro transfer and lifecycle checks. CrossPoint
tests the four-tone pixel encoding and the separate image/AA quantizers. On hardware, compare AA page turns,
image pages, inverted reading, and the first page after wake. Include Paper Mono
for combined-base behavior; the capability refactor changes no waveform tables.

## Native image level count and sixteen-level transactions

`BoardProfile::grayscaleLevels` is a trailing `uint8_t` with default `4`, so old
aggregate profiles keep their initialization and behavior. Only `READ_PICO`
sets `16`. `FreeInkDisplay::getGrayscaleLevels()` reports that board capability;
HalDisplay and GfxRenderer expose it to the application. This does not change
Overlay/Absolute encoding or reader text AA. The simulator still reports four.

`beginGrayscale16()` drains an existing asynchronous refresh, rejects inversion
and duplicate loans, and borrows the driver's existing front framebuffer. A null
pointer means unavailable; other PanelDrivers default to unsupported and allocate
nothing. Storage contains two pixels per byte: even X in the low nibble, odd X in
the high nibble, `0` black through `15` white. Physical dimensions come from the
active profile. The application owns the exclusive loan until commit/cancel and
must maintain its existing 1bpp B/W proxy; no other drawing/display transaction
may run during the loan.

`commitGrayscale16()` consumes the loan and returns the actual backend refresh
result. EpdiyLcd uses GL16 for ordinary image commits. Unknown baselines and failed
refresh recovery retain the existing clear/GC16 policy. Successful commits stash
the B/W proxy for later four-level AA. `cancelGrayscale16()` makes no panel write,
restores a known front baseline from the existing back buffer, and leaves an
unknown baseline unknown. Sleep/end release the loan with the existing buffers.
No additional full-screen native buffer is allocated.

`libs/display/EpdiyLcd/test/host/test_transactions.py` was run on Linux for this
change (diagnostics on/off). It covers native nibble packing, zero allocations
while borrowing, cancellation, GL16, draw failure/GC16 retry, existing seven
allocation failures, sleep/reinitialization, queues and concurrent PMU operations.
The macOS linker does not support the harness's `--wrap=calloc`. This software
result is separate from optical/electrical acceptance. CrossMux's 2026-10-01
Read Pico app0 image SHA-256 is
`e01c746963db6df1053cff8eb3018d7c6a8f90da9ef3c24010677d77736f0cba`;
write verification passed. Idle internal free heap was 75,679 bytes with a
31,732-byte largest block. The user confirmed sixteen distinguishable tones,
normal polarity, the saved sleep cover, modal dismissal and wake. All four
orientations, repeated sleep/wake cycles and long-term ghosting still require
separate physical checks; this is user observation, not optical measurement.
