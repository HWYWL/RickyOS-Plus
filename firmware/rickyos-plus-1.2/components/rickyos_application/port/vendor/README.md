# stb_image

`stb_image.h` is the v2.30 header from nothings/stb commit
`a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d`.

Source: https://github.com/nothings/stb/blob/a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d/stb_image.h

Original SHA256: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.

Local modification: optional `STBI_RICKYOS_JPEG_CHECKPOINT()` (default true) at
progressive entropy row starts and final IDCT row starts. It yields through the
existing decoder budget and exits on cancellation. The owner rejects cancelled
results and stb's existing failure cleanup releases coefficients. No color,
IDCT, entropy or resampling arithmetic is changed.

Patched SHA256: `63740a647de8e20bb2699d28829e8d482c90d3ddb0bf275c67380edff2a63be9`.

Used under the MIT option reproduced at the end of the header. Original copyright
and complete dual-license text are retained. Only JPEG is compiled, privately in
`PlusProgressiveJpeg.cpp`; PNG/other formats, stdio, HDR and SIMD are disabled.
This is a software decoder, not ESP32-S31 JPEG hardware acceleration.
