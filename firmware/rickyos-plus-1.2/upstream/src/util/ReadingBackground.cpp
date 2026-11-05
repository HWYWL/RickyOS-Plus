#include "ReadingBackground.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"

namespace ReadingBackground {
namespace {
// How much of the picture's darkness is kept: light, medium, original. The last is the
// picture as it is (users found even the old deepest step, 55%, too pale to see).
constexpr uint8_t kInkPercent[] = {0, 30, 60, 100};
static_assert(std::size(kInkPercent) == CrossPointSettings::READING_BACKGROUND_COUNT);
// Ruled paper is a few thin marks, not a picture: it needs a darker gray and a little
// more weight than a lightened picture to be seen at all (level, line, grid line, dot).
struct PaperInk {
  uint8_t level;
  uint8_t line;
  uint8_t grid;
  uint8_t dot;
};
constexpr PaperInk kPaperInk[] = {{15, 0, 0, 0}, {10, 2, 2, 4}, {7, 2, 2, 5}, {2, 3, 2, 6}};
static_assert(std::size(kPaperInk) == CrossPointSettings::READING_BACKGROUND_COUNT);

int8_t present = -1;  // the file exists: -1 unknown
uint32_t generation = 0;

// The drawn background for one screen shape, paper and strength, reused for every page.
struct Copy {
  uint8_t* frame = nullptr;
  size_t bytes = 0;
  int orientation = -1;
  uint8_t strength = 0;
  uint8_t paper = 0;
  TextBox box;
  uint32_t generation = UINT32_MAX;
} copy;

bool sameBox(const TextBox& a, const TextBox& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom && a.pitch == b.pitch &&
         a.rule == b.rule;
}

bool picturePaper() { return SETTINGS.readingPaper == CrossPointSettings::READING_PAPER_PICTURE; }

bool drawPicture(const GfxRenderer& renderer, uint8_t* frame, const size_t bytes, const uint8_t strength) {
  HalFile file;
  if (!Storage.openFileForRead("READBG", kPath, file)) {
    present = 0;
    return false;
  }
  Bitmap bitmap(file, false);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      !bitmap.hasGreyscale()) {
    LOG_ERR("READBG", "Background picture unreadable");
    return false;
  }
  // Fill the screen and crop the middle, as a cover is drawn.
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const float scale = std::max(static_cast<float>(screenWidth) / bitmap.getWidth(),
                               static_cast<float>(screenHeight) / bitmap.getHeight());
  const int width = static_cast<int>(std::lround(bitmap.getWidth() * scale));
  const int height = static_cast<int>(std::lround(bitmap.getHeight() * scale));
  bool drawn = false;
  {
    const GfxRenderer::ClipScope clip(renderer, 0, 0, screenWidth, screenHeight);
    drawn = renderer.drawBitmapGrayscale16(bitmap, (screenWidth - width) / 2, (screenHeight - height) / 2, width,
                                           height);
  }
  if (!drawn) {
    memset(frame, 0xFF, bytes);
    LOG_ERR("READBG", "Background picture draw failed");
    return false;
  }
  // Two pixels a byte: lighten both nibbles through one table.
  uint8_t table[256];
  for (int value = 0; value < 256; ++value)
    table[value] = static_cast<uint8_t>(lighten(value & 15, strength) | (lighten(value >> 4, strength) << 4));
  for (size_t i = 0; i < bytes; ++i) frame[i] = table[frame[i]];
  return true;
}
}  // namespace

bool active() {
  if (SETTINGS.readingBackground == CrossPointSettings::READING_BACKGROUND_OFF) return false;
  if (!picturePaper()) return true;
  if (present < 0) present = Storage.exists(kPath) ? 1 : 0;
  return present == 1;
}

void invalidate() {
  present = -1;
  ++generation;
}

uint8_t lighten(const uint8_t level, const uint8_t strength) {
  const int ink = kInkPercent[std::min<int>(strength, std::size(kInkPercent) - 1)];
  const int dark = 15 - std::min<int>(level, 15);
  return static_cast<uint8_t>(15 - (dark * ink + 50) / 100);
}

void drawPaper(const GfxRenderer& renderer, const uint8_t paper, const uint8_t strength, const TextBox& box) {
  if (box.pitch <= 0 || box.right <= box.left || box.bottom <= box.top) return;
  const PaperInk& ink = kPaperInk[std::min<int>(strength, std::size(kPaperInk) - 1)];
  if (ink.level >= 15) return;
  const uint8_t level = ink.level;
  const bool gray = renderer.isGrayscale16Active();
  const auto rect = [&](const int x, const int y, const int width, const int height) {
    if (gray) {
      renderer.fillGrayscale16Rect(x, y, width, height, level);
    } else {
      renderer.fillRect(x, y, width, height, true);
    }
  };
  // One rule per text row, where the reading guide line sits under the glyphs.
  const int width = box.right - box.left;
  const int firstRule = box.top + box.rule;
  switch (paper) {
    case CrossPointSettings::READING_PAPER_LINES:
      for (int y = firstRule; y + ink.line <= box.bottom; y += box.pitch) rect(box.left, y, width, ink.line);
      break;
    case CrossPointSettings::READING_PAPER_GRID: {
      // Square cells a row high; the columns start at the text's left edge.
      const int gridTop = std::max(box.top, firstRule - box.pitch);
      int gridBottom = gridTop;
      for (int y = firstRule - box.pitch; y + ink.grid <= box.bottom; y += box.pitch) {
        if (y < box.top) continue;
        rect(box.left, y, width, ink.grid);
        gridBottom = y + ink.grid - 1;
      }
      for (int x = box.left; x + ink.grid <= box.right + 1; x += box.pitch)
        rect(x, gridTop, ink.grid, gridBottom - gridTop + 1);
      break;
    }
    case CrossPointSettings::READING_PAPER_DOTS:
      // A dot on every grid crossing.
      // Centred on the crossing.
      for (int y = firstRule; y + ink.dot / 2 <= box.bottom; y += box.pitch)
        for (int x = box.left; x + ink.dot / 2 <= box.right; x += box.pitch)
          rect(x - ink.dot / 2, y - ink.dot / 2, ink.dot, ink.dot);
      break;
    default:
      break;
  }
}

bool paint(const GfxRenderer& renderer, const TextBox& box) {
  if (!active()) return false;
  uint8_t* frame = renderer.grayscale16Frame();
  const size_t bytes = renderer.grayscale16FrameBytes();
  if (!frame || bytes == 0) return false;
  const uint8_t strength = SETTINGS.readingBackground;
  const uint8_t paper = SETTINGS.readingPaper;
  if (!picturePaper() && box.pitch <= 0) return false;  // ruled paper follows horizontal rows only
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (copy.frame && copy.bytes == bytes && copy.orientation == orientation && copy.strength == strength &&
      copy.paper == paper && copy.generation == generation && (picturePaper() || sameBox(copy.box, box))) {
    memcpy(frame, copy.frame, bytes);
    return true;
  }

  if (picturePaper()) {
    if (!drawPicture(renderer, frame, bytes, strength)) return false;
  } else {
    memset(frame, 0xFF, bytes);
    drawPaper(renderer, paper, strength, box);
  }
  // The proxy took the background's own darks; popups over the page expect paper.
  memset(renderer.getFrameBuffer(), 0xFF, renderer.getBufferSize());

  if (copy.bytes != bytes) {
    heap_caps_free(copy.frame);
    copy.frame = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    copy.bytes = copy.frame ? bytes : 0;
  }
  if (copy.frame) {
    memcpy(copy.frame, frame, bytes);
    copy.orientation = orientation;
    copy.strength = strength;
    copy.paper = paper;
    copy.box = box;
    copy.generation = generation;
  }
  return true;
}

}  // namespace ReadingBackground
