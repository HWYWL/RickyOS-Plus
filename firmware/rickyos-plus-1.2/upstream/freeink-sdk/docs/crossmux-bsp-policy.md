# CrossMux BSP alignment

Reference SDK: `587695442525245dcf9322903c92f72731d11494`.
Reference reader: `6f94d1ad5d84a81ec8a28c06b0afe01a7300fe75`.

## Official reader devices

X3 (UC8253 and UC8279), X4, X4 Pro, X4 Classic, Sticky and Paper Mono
use the upstream board profile values and panel drivers. SSD1677, UC8179,
UC8253X3, UC8279 and UC8279X4 driver sources match the reference SDK;
Paper Mono retains the upstream driver. Runtime controller detection and SPI
selection also remain upstream behavior.

The facade retains source-compatible refresh-context overloads for CrossMux.
For official drivers these forward to the original entry points: continuous
reading and dark-background hints do not replace upstream waveforms or cleanup.
Absolute/direct capabilities, upload cancellation and post-gray recovery come
from the upstream drivers. These capabilities require the consumer HAL and
renderer to select the corresponding mode; updating the SDK alone does not
change every sleep-image rendering path.

## Independent local boards

`CrossMuxSsd1677Driver` isolates the previously validated Murphy M4, local
Waveshare 3.97 and Metalio E-Ink 4 sequences. Its implementation is compiled
only for those devices, and only their profiles select it. It retains the
local refresh context, dark-background hint, batch calibration and power-off
behavior. Absolute grayscale remains disabled for these local profiles.
EEGO A4 retains its separate UC8279C driver, touch implementation and wake latch.
The SDK WS397 entry remains distinct from CrossMux WAVESHARE_EPAPER_397.

The copied SSD1677 backend deliberately preserves the existing local sequence
implementation while keeping the upstream backend independently reviewable;
future upstream driver changes must be reviewed for local applicability, not
blindly copied into this backend.

## CrossMux extensions

Physical press observation and consumed touch-edge clearing are additive input
interfaces for the consumer's logical input ownership. They do not change the
upstream official-board sampling, debounce, gestures or touch decoding.
Local touch, power, audio and RTC drivers remain selected by local profiles.
Previously approved capacity-query fixes are retained.

Standby-face timed light sleep, BLE power guards and concurrency protection
belong to the consumer HAL, outside the ordinary upstream deep-sleep path.
No new full-frame allocation is introduced by this alignment.

## Acceptance

Host command-sequence tests and official-source/profile comparisons establish
software alignment only. Physical residual-image, sleep-current and wake
acceptance must be recorded by device and panel batch. In particular Metalio
keeps its previous calibration; this change does not claim to fix its ghosting.
