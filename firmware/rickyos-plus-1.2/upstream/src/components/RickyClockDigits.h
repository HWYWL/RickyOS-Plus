#pragma once

class GfxRenderer;

// The Standby clock: large anti-aliased digits blended into the active 16-gray frame
// (src/components/fonts/rickyClockDigits.h). Callers check isGrayscale16Active().
namespace RickyClockDigits {

int capHeight();
// Width of "HH:MM"-style text drawn by draw(); digits share one cell width.
int width(const char* text);
// Blend text into the 16-gray frame with its digits' top at yTop.
void draw(GfxRenderer& renderer, int x, int yTop, const char* text);
// Pure B/W digits for faces refreshed with the quiet B/W waveform: the coverage is
// scaled by `scalePercent` with bilinear sampling, then thresholded, so the outline
// stays smooth at sizes the anti-aliased set was not drawn for. widthBw() matches.
int widthBw(const char* text, int scalePercent);
int capHeightBw(int scalePercent);
void drawBw(GfxRenderer& renderer, int x, int yTop, const char* text, int scalePercent);
// Fade the picture towards paper from transparent at `top` to `strength` (0..15 of 15)
// at `fullAt`, keeping that strength down to `bottom`: the clock stays legible over
// any picture without a box around it.
void fadeToPaper(GfxRenderer& renderer, int left, int top, int width, int fullAt, int bottom, int strength);

}  // namespace RickyClockDigits
