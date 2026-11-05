#include "ReaderChrome.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "SdCardFontSystem.h"
#include "components/RickyCompanion.h"
#include "components/icons/rickyReaderCat.h"
#include "components/icons/rickyReaderDog.h"
#include "fontIds.h"
#include "util/ReadingGuideLine.h"

namespace readerChrome {
namespace {

using readingMode::Box;

#if FREEINK_DEVICE_READPICO
constexpr int kLabelFontId = READER_STATUS_FONT_ID;
#else
constexpr int kLabelFontId = SMALL_FONT_ID;
#endif

// The dog's 2-bit art with its coverage mask: covered pixels are written (paper
// included, which hides the frame line under the head); the rest stay untouched.
// The 16-level pass gets the art's grays; B/W passes threshold at half ink.
struct ReaderArt {
  int kWidth, kHeight, kGrayStride, kMaskStride;
  const uint8_t *kGray, *kMask;
};
// Both companions stand on the same edge row, so the reading margins never depend on the theme.
static_assert(RickyReaderCat::kEdgeRow == RickyReaderDog::kEdgeRow, "companion edge rows");
ReaderArt readerArt() {
  if (RickyCompanion::cat())
    return {RickyReaderCat::kWidth, RickyReaderCat::kHeight, RickyReaderCat::kGrayStride, RickyReaderCat::kMaskStride,
            RickyReaderCat::kGray,  RickyReaderCat::kMask};
  return {RickyReaderDog::kWidth, RickyReaderDog::kHeight, RickyReaderDog::kGrayStride, RickyReaderDog::kMaskStride,
          RickyReaderDog::kGray,  RickyReaderDog::kMask};
}

void drawDog(const GfxRenderer& renderer, const int originX, const int originY) {
  const auto [kWidth, kHeight, kGrayStride, kMaskStride, kGray, kMask] = readerArt();
  const bool gray = renderer.isGrayscale16Active();
  for (int y = 0; y < kHeight; ++y) {
    const uint8_t* maskRow = kMask + y * kMaskStride;
    const uint8_t* grayRow = kGray + y * kGrayStride;
    for (int x = 0; x < kWidth; ++x) {
      if ((maskRow[x / 8] & (0x80U >> (x % 8))) == 0) continue;
      const uint8_t level = (grayRow[x / 4] >> (6 - 2 * (x % 4))) & 0x03U;
      if (gray) {
        renderer.drawGrayscale16Pixel(originX + x, originY + y, static_cast<uint8_t>(level * 85));
      } else {
        renderer.drawPixel(originX + x, originY + y, level <= 1);
      }
    }
  }
}

void drawPuppy(const GfxRenderer& renderer, const Box& text) {
  const Box frame = readingMode::puppyFrame(text);
  renderer.drawRoundedRect(frame.x, frame.y, frame.width, frame.height, readingMode::kHairline,
                           readingMode::kPuppyRadius, true);
  const int dogX = frame.right() - readingMode::kPuppyDogInset - readerArt().kWidth;
  // The art's edge row lands on the frame line's lower row, so paws and tongue hang over it.
  const int dogY = frame.y + readingMode::kHairline - 1 - RickyReaderDog::kEdgeRow;
  if (dogX > frame.x + readingMode::kPuppyRadius) drawDog(renderer, dogX, dogY);
}

// 鱼尾: a block with a forked edge. `down` points the fork downward (the upper fish tail).
void drawFishTail(const GfxRenderer& renderer, const Box& column, const int y, const int height, const bool down) {
  constexpr int kSideInset = 4;
  const int left = column.x + kSideInset;
  const int right = column.right() - kSideInset - 1;
  const int mid = column.x + column.width / 2;
  const int notch = height * 9 / 20;  // depth of the fork
  if (down) {
    const int xs[] = {left, right, right, mid, left};
    const int ys[] = {y, y, y + height, y + height - notch, y + height};
    renderer.fillPolygon(xs, ys, 5, true);
  } else {
    const int xs[] = {left, mid, right, right, left};
    const int ys[] = {y, y + notch, y, y + height, y + height};
    renderer.fillPolygon(xs, ys, 5, true);
  }
}

// One glyph per row, centred on the column: 版心 text runs top to bottom.
// Returns the y below the last glyph drawn; stops before `limit`.
int drawVertical(const GfxRenderer& renderer, const char* text, const int centerX, int y, const int step,
                 const int limit) {
  if (text == nullptr) return y;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  char glyph[8];
  while (*cursor != 0 && y + step <= limit) {
    const unsigned char* start = cursor;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    const size_t length = std::min<size_t>(static_cast<size_t>(cursor - start), sizeof(glyph) - 1);
    if (cp == ' ' || cp == 0x3000) {  // spaces keep a half step
      y += step / 2;
      continue;
    }
    memcpy(glyph, start, length);
    glyph[length] = '\0';
    const int width = renderer.getTextWidth(kLabelFontId, glyph);
    renderer.drawText(kLabelFontId, centerX - width / 2, y, glyph);
    y += step;
  }
  return y;
}

int verticalHeight(const char* text, const int step) {
  if (text == nullptr) return 0;
  int height = 0;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor != 0) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    height += (cp == ' ' || cp == 0x3000) ? step / 2 : step;
  }
  return height;
}

void drawAncient(const GfxRenderer& renderer, const Box& text, const PageInfo& page) {
  using namespace readingMode;
  const Box frame = ancientFrame(text);
  // 四周双边: thick outer rule, thin inner rule.
  renderer.drawRect(frame.x, frame.y, frame.width, frame.height, kEmphasis, true);
  const int inner = kAncientRules - kHairline;
  renderer.drawRect(frame.x + inner, frame.y + inner, frame.width - 2 * inner, frame.height - 2 * inner, kHairline,
                    true);
  const Box column = banxin(text);
  // The rule between the 版心 and the text.
  renderer.fillRect(column.right(), column.y, kHairline, column.height, true);

  // 黑口 at both ends, then the fish tails facing each other.
  constexpr int kMouth = 28;
  constexpr int kMouthWidth = 6;
  constexpr int kTail = 22;
  constexpr int kGap = 10;
  const int centerX = column.x + column.width / 2;
  renderer.fillRect(centerX - kMouthWidth / 2, column.y, kMouthWidth, kMouth, true);
  renderer.fillRect(centerX - kMouthWidth / 2, column.bottom() - kMouth, kMouthWidth, kMouth, true);
  const int upperTail = column.y + kMouth;
  const int lowerTail = column.bottom() - kMouth - kTail;
  drawFishTail(renderer, column, upperTail, kTail, true);
  drawFishTail(renderer, column, lowerTail, kTail, false);

  const int glyph = std::max(1, renderer.getTextWidth(kLabelFontId, "国"));
  const int step = glyph + 3;
  // Page number sits on the lower fish tail; the title fills what is left above it.
  char number[32];
  readingMode::chineseNumeral(std::max(1, page.pageInChapter), number, sizeof(number));
  const int numberTop = lowerTail - kGap - verticalHeight(number, step);
  drawVertical(renderer, number, centerX, numberTop, step, lowerTail);
  const char* label = page.chapterTitle && page.chapterTitle[0] ? page.chapterTitle : page.bookTitle;
  drawVertical(renderer, label, centerX, upperTail + kTail + kGap, step, numberTop - 2 * kGap);
}

// One 界行: a gray hairline on the 16-level frame (never lightening ink), ink in B/W.
void drawColumnRule(const GfxRenderer& renderer, const int x, const int top, const int bottom) {
  const bool gray = renderer.isGrayscale16Active();
  for (int dx = 0; dx < readingMode::kHairline; ++dx) {
    for (int y = top; y < bottom; ++y) {
      readingGuideLine::detail::plot(renderer, x + dx, y, readingGuideLine::kLineGray, gray);
    }
  }
}

}  // namespace

void drawColumnRules(const GfxRenderer& renderer, const Page& page, const readingMode::Box& text, const int pitch) {
  if (pitch <= 0 || text.width <= 0) return;
  const int top = text.y - readingMode::kTextPad;
  const int bottom = text.bottom() + readingMode::kTextPad;
  const int right = text.right();
  // The frame's rules close both ends; a 界行 sits on every column edge between them,
  // centred on the edge.
  const auto rule = [&](const int edge) {
    if (edge > text.x + readingMode::kHairline && edge < right - readingMode::kHairline) {
      drawColumnRule(renderer, edge - readingMode::kHairline / 2, top, bottom);
    }
  };
  int leftmost = right;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine && element->getTag() != TAG_PageHorizontalRule) continue;
    const int columnRight = right - element->yPos;
    rule(columnRight);
    rule(columnRight - pitch);
    leftmost = std::min(leftmost, columnRight - pitch);
  }
  // Empty columns after the text keep the grid, as on a printed leaf.
  for (int edge = leftmost - pitch; edge > text.x; edge -= pitch) rule(edge);
}

void draw(const GfxRenderer& renderer, const readingMode::Mode mode, const readingMode::Box& text,
          const PageInfo& page) {
  if (text.width <= 0 || text.height <= 0) return;
  switch (mode) {
    case readingMode::Mode::Puppy:
      drawPuppy(renderer, text);
      break;
    case readingMode::Mode::Ancient:
      drawAncient(renderer, text, page);
      break;
    case readingMode::Mode::Original:
    case readingMode::Mode::Immersive:
    case readingMode::Mode::Count:
      break;
  }
}

}  // namespace readerChrome
