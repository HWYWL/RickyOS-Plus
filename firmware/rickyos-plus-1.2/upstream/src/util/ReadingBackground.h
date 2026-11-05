#pragma once

#include <cstdint>

class GfxRenderer;

// 阅读背景: what lies under the reading page, lightened so the text stays the darkest
// thing on it, or at 原色 drawn in its own tones. Either a picture (/readingbg.bmp, converted like /sleep.bmp) or ruled
// paper (lines, a grid or dots) drawn to the rows of the text. The setting chooses how
// light it is drawn, or turns it off. Only 16-level pages carry it (not Night mode).
namespace ReadingBackground {

constexpr char kPath[] = "/readingbg.bmp";

// Where the text sits, so ruled paper lines up with it. Screen coordinates; `pitch` is
// the body line height and `rule` how far below a line's top its ruling falls. A zero
// pitch (vertical text) leaves ruled paper out.
struct TextBox {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int pitch = 0;
  int rule = 0;
};

// On, with something to show. The picture's file is checked once until invalidate().
bool active();
// The picture or the setting changed: check the file again and redraw the copy.
void invalidate();

// A 4-bit level as drawn at `strength` (READING_BACKGROUND_*): white stays white,
// black comes out as a light or mid gray, or stays black at the original strength.
uint8_t lighten(uint8_t level, uint8_t strength);

// Ruled paper (READING_PAPER_*) into the active 16-level frame, at `strength`.
void drawPaper(const GfxRenderer& renderer, uint8_t paper, uint8_t strength, const TextBox& box);

// Inside a 16-level pass, right after its clear and before the page: fill the frame
// with the background. The first call draws it; later ones copy the result kept in
// PSRAM while the paper, shade, orientation and text box stay the same. The B/W proxy
// stays paper. False when inactive or failed.
bool paint(const GfxRenderer& renderer, const TextBox& box);

}  // namespace ReadingBackground
