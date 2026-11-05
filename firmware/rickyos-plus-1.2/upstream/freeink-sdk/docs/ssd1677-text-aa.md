# SSD1677 text-only combined AA

`FREEINK_SSD1677_COMBINED_AA` defaults on for Sticky, Murphy M4, CrossMux
Waveshare ePaper 3.97 and Metalio E-Ink4. X4 Pro and X4 Classic default off,
restoring their original B/W base plus gray overlay without the eight combined-AA
buffers; an explicit build override remains available. The facade requires
ESP32-S3, the detected SSD1677 controller, 800x480 geometry and successful PSRAM
allocation. UC controllers and ESP32-C3 retain their previous paths.

Only a grayscale base requested with `RefreshContext::TextOnlyAntiAliasing`
selects the PaperMono-derived three-tone engine. `supportsTextOnlyCombinedBase()`
reports availability before staging; `combinesGrayscaleBase()` reports the
active text transaction. The Overlay encoding and existing strip interface
remain unchanged. Absolute/Direct requests retain the original backend.

Images and unmarked callers keep their original driver, LUT and two distinct
intermediate gray planes. Text/image switches wait for outstanding work,
invalidate the RAM baseline and either use a trusted full-target AA handoff
or perform the board's normal correction. Repeated
text pages share one pixel activation; repeated image pages retain their
original refresh sequence. A controller parked by idle sleep is reset before
BUSY is checked and the next page touches RAM. Paper Mono retains its existing
behavior, with an SSD1677 fallback if its eight PSRAM allocations fail.

`src/lut/Ssd1677CombinedAa.h` holds separate calibration entries. Sticky's
validated timing is unchanged; other boards start at nominal 5ms frames and
16/24/32 kick/gray/black counts, with the original gray voltage tails. Their
optical behavior is not yet validated. The original drivers still implement
corrective B/W, including Murphy batch selection and Metalio black-pulse
cleaning. Native RAM direction follows the board, not Paper Mono's mount.

## Trusted reader transitions

`FREEINK_SSD1677_READER_TRANSITIONS` defaults on only for Metalio, Sticky,
Murphy M4 and Waveshare 3.97 when combined text routing is enabled. Set it to 0
to keep combined AA but restore prepass transitions; set
`FREEINK_SSD1677_COMBINED_AA=0` to use the
original driver throughout. Other boards keep their existing transition policy.
The old Metalio experiment flags are no longer used.

`supportsReaderTransitions()` reports the capability through the facade/HAL.
`canUseTextTransition()` requires successful original-driver B/W or gray optical
history, settled BUSY and normal polarity; drain pending work before querying.
The SDK captures permission before the driver handoff and checks it again after
power-down; callers submit the existing text-AA context. Only a FAST transition
with complete AA planes omits the OTP prepass.
It runs the existing **full-target corrective AA waveform**; LUT bytes, ordinary
text drive and framebuffer allocations are unchanged. A failed power-down,
unknown history, resync, missing planes or canceled transaction cannot reuse the
permission. Explicit HALF/FULL always retain correction.

Optical history is independent of RAM synchronization and gray-clean flags.
Both original drivers record successful B/W/gray operations, retain gray history
after RAM-only cleanup, and invalidate history on initialization, resync or
failed refresh. Async history becomes valid only at completion. Driver handoff
may carry optical history across controller sleep; a new facade session may not.
Murphy M4 retains each batch's original temperature parameters.

Each original driver's completion check handles BUSY failure in one place and
discards both current and deferred optical history. A late completion cannot
revive a failed frame. This protection also applies when transitions are disabled.
Per-activation instrumentation is restricted to `SSD1677_PROBE_DEBUG` builds.

`supportsContinuousImageReading()` is separate and true only for enabled
Metalio. Image bases there consume the reader's existing cadence and use a white
endpoint when cleanup is required; text/menu endpoints remain black. Sticky,
Murphy M4 and Waveshare 3.97 keep their original image cadence, LUT and cleanup
sequences.

EPUB/TXT entry counter zero starts a new configured reading cycle after a
trusted entry. Counter one is real cleanup debt and is deferred only to the next
ordinary page; manual cleanup is never deferred. Frequency one still cleans on
each following page. No user setting or cache migration is introduced.

Eight 48,000-byte PSRAM planes persist for the driver lifetime. This page/glass
history cannot live on the worker stack. No additional full-page cache is
allocated; partial OOM frees every allocated plane and retains the original
route. Incomplete gray data falls back to B/W; canceled generations are dropped.
BUSY timeout stops writes and forces baseline resynchronization on recovery.

Run the host checks from `libs/display/FreeInkDisplay/test/host`:
`test_sticky_combined_aa.py`, `test_ssd1677_text_route.py`, `test_ssd1677.py`
and `run_pro.py`. Tests compare complete original image command/data traces,
exercise both Murphy batches, scan orientation, power/sleep, OOM and timeouts.
CrossMux's physical acceptance record and packaging instructions live in
`docs/engineering/ssd1677-text-aa.md` in the parent project. A single controller
activation is not itself proof of optically flicker-free output.
