"""阅读背景: the actual ReadingBackground.cpp against a recording renderer, plus the reader wiring."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT / 'firmware/rickyos-plus-1.2/upstream'
sys.path.insert(0, str(ROOT / 'scripts/tests'))
from test_reading_ui_regressions import method

STUBS = {
    'CrossPointSettings.h': r'''
#pragma once
#include <cstdint>
struct CrossPointSettings {
  enum READING_BACKGROUND : uint8_t {READING_BACKGROUND_OFF = 0, READING_BACKGROUND_LIGHT, READING_BACKGROUND_MEDIUM,
                                     READING_BACKGROUND_DEEP, READING_BACKGROUND_COUNT};
  enum READING_PAPER : uint8_t {READING_PAPER_PICTURE = 0, READING_PAPER_LINES, READING_PAPER_GRID, READING_PAPER_DOTS,
                                READING_PAPER_COUNT};
  uint8_t readingBackground = READING_BACKGROUND_OFF;
  uint8_t readingPaper = READING_PAPER_PICTURE;
};
inline CrossPointSettings SETTINGS;
''',
    'Logging.h': '#pragma once\n#define LOG_ERR(...) ((void)0)\n',
    'esp_heap_caps.h': r'''
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline int heapAllocations = 0;
inline void* heap_caps_malloc(std::size_t size, unsigned) { ++heapAllocations; return std::malloc(size); }
inline void heap_caps_free(void* p) { std::free(p); }
''',
    'HalStorage.h': r'''
#pragma once
#include <set>
#include <string>
struct HalFile {};
struct StorageStub {
  std::set<std::string> files;
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool openFileForRead(const char*, const char* path, HalFile&) const { return exists(path); }
};
inline StorageStub Storage;
''',
    'Bitmap.h': r'''
#pragma once
#include "HalStorage.h"
enum class BmpReaderError { Ok, Bad };
struct Bitmap {
  Bitmap(HalFile&, bool) {}
  BmpReaderError parseHeaders() const { return BmpReaderError::Ok; }
  int getWidth() const { return 300; }
  int getHeight() const { return 400; }
  bool hasGreyscale() const { return true; }
};
''',
    'GfxRenderer.h': r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Bitmap.h"
// A 16-level frame the size of the screen, one level per pixel kept apart for checks,
// packed two to a byte in the frame the background code copies.
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise };
  static constexpr int W = 640, H = 960;
  mutable std::vector<uint8_t> packed = std::vector<uint8_t>(W / 2 * H, 0xFF);
  mutable std::vector<uint8_t> bw = std::vector<uint8_t>(W / 8 * H, 0);
  mutable int fills = 0, pictures = 0;
  bool gray = true;
  Orientation orientation = Portrait;
  int getScreenWidth() const { return W; }
  int getScreenHeight() const { return H; }
  Orientation getOrientation() const { return orientation; }
  bool isGrayscale16Active() const { return gray; }
  uint8_t* grayscale16Frame() const { return gray ? packed.data() : nullptr; }
  size_t grayscale16FrameBytes() const { return packed.size(); }
  uint8_t* getFrameBuffer() const { return bw.data(); }
  size_t getBufferSize() const { return bw.size(); }
  uint8_t level(int x, int y) const {
    const uint8_t byte = packed[(y * W + x) / 2];
    return x % 2 ? byte >> 4 : byte & 15;
  }
  void put(int x, int y, uint8_t value) const {
    uint8_t& byte = packed[(y * W + x) / 2];
    byte = x % 2 ? static_cast<uint8_t>((byte & 0x0F) | (value << 4)) : static_cast<uint8_t>((byte & 0xF0) | value);
  }
  void fillGrayscale16Rect(int x, int y, int width, int height, uint8_t value) const {
    ++fills;
    for (int yy = y; yy < y + height; ++yy)
      for (int xx = x; xx < x + width; ++xx)
        if (xx >= 0 && yy >= 0 && xx < W && yy < H) put(xx, yy, value);
  }
  void fillRect(int, int, int, int, bool) const { ++fills; }
  bool drawBitmapGrayscale16(const Bitmap&, int, int, int, int) const {
    ++pictures;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) put(x, y, 0);  // a black picture
    return true;
  }
  struct ClipScope {
    ClipScope(const GfxRenderer&, int, int, int, int) {}
  };
};
''',
}

PROGRAM = r'''
#include <cassert>
#include "util/ReadingBackground.h"
#include "GfxRenderer.h"
#include "CrossPointSettings.h"
#include "esp_heap_caps.h"
using S = CrossPointSettings;
namespace RB = ReadingBackground;

int ink(const GfxRenderer& r, int x0, int y0, int x1, int y1) {
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) n += r.level(x, y) != 15;
  return n;
}

int main() {
  // White stays white; black comes out lighter at each shade, darker as the shade deepens.
  for (int s = 0; s < S::READING_BACKGROUND_COUNT; ++s) assert(RB::lighten(15, s) == 15);
  assert(RB::lighten(0, S::READING_BACKGROUND_OFF) == 15);
  assert(RB::lighten(0, S::READING_BACKGROUND_LIGHT) > RB::lighten(0, S::READING_BACKGROUND_MEDIUM));
  assert(RB::lighten(0, S::READING_BACKGROUND_MEDIUM) > RB::lighten(0, S::READING_BACKGROUND_DEEP));
  // 原色 (the old 深): the picture in its own tones, unlightened.
  for (int level = 0; level < 16; ++level) assert(RB::lighten(level, S::READING_BACKGROUND_DEEP) == level);

  GfxRenderer r;
  // A text box with 50 px rows whose rule falls 38 px below each row's top.
  const RB::TextBox box{40, 100, 600, 900, 50, 38};

  // Lines: one rule under every row, inside the text box only.
  r = GfxRenderer{};
  RB::drawPaper(r, S::READING_PAPER_LINES, S::READING_BACKGROUND_MEDIUM, box);
  for (int y = 0; y < GfxRenderer::H; ++y) {
    const bool rule = y >= box.top && y < box.bottom && (y - box.top - box.rule) % box.pitch >= 0 &&
                      (y - box.top - box.rule) % box.pitch < 2 && y >= box.top + box.rule;
    assert((r.level(box.left + 7, y) != 15) == rule);
  }
  assert(ink(r, 0, 0, box.left, GfxRenderer::H) == 0 && ink(r, box.right, 0, GfxRenderer::W, GfxRenderer::H) == 0);
  assert(ink(r, 0, 0, GfxRenderer::W, box.top) == 0 && ink(r, 0, box.bottom, GfxRenderer::W, GfxRenderer::H) == 0);
  const uint8_t lineLevel = r.level(box.left, box.top + box.rule);
  // Deeper shades draw ruled paper darker; light still shows.
  r = GfxRenderer{};
  RB::drawPaper(r, S::READING_PAPER_LINES, S::READING_BACKGROUND_DEEP, box);
  assert(r.level(box.left, box.top + box.rule) < lineLevel);
  r = GfxRenderer{};
  RB::drawPaper(r, S::READING_PAPER_LINES, S::READING_BACKGROUND_LIGHT, box);
  assert(r.level(box.left, box.top + box.rule) > lineLevel && r.level(box.left, box.top + box.rule) < 15);
  r = GfxRenderer{};
  RB::drawPaper(r, S::READING_PAPER_LINES, S::READING_BACKGROUND_OFF, box);
  assert(ink(r, 0, 0, GfxRenderer::W, GfxRenderer::H) == 0);

  // Grid: the same rules plus columns a row apart from the text's left edge.
  r = GfxRenderer{};
  RB::drawPaper(r, S::READING_PAPER_GRID, S::READING_BACKGROUND_MEDIUM, box);
  assert(r.level(box.left + 7, box.top + box.rule) != 15);
  assert(r.level(box.left + box.pitch, box.top + box.rule + 20) != 15);
  assert(r.level(box.left + box.pitch + 10, box.top + box.rule + 20) == 15);
  assert(ink(r, 0, 0, GfxRenderer::W, box.top) == 0 && ink(r, 0, box.bottom, GfxRenderer::W, GfxRenderer::H) == 0);

  // Dots: on the grid's crossings, nothing between them.
  r = GfxRenderer{};
  RB::drawPaper(r, S::READING_PAPER_DOTS, S::READING_BACKGROUND_MEDIUM, box);
  assert(r.level(box.left + box.pitch, box.top + box.rule + box.pitch) != 15);
  assert(r.level(box.left + box.pitch + box.pitch / 2, box.top + box.rule + box.pitch) == 15);
  assert(r.level(box.left + box.pitch, box.top + box.rule + box.pitch / 2) == 15);

  // Vertical text has no rows: no ruled paper.
  r = GfxRenderer{};
  RB::TextBox columns = box;
  columns.pitch = 0;
  RB::drawPaper(r, S::READING_PAPER_LINES, S::READING_BACKGROUND_DEEP, columns);
  assert(ink(r, 0, 0, GfxRenderer::W, GfxRenderer::H) == 0);

  // paint(): off draws nothing.
  r = GfxRenderer{};
  SETTINGS.readingBackground = S::READING_BACKGROUND_OFF;
  SETTINGS.readingPaper = S::READING_PAPER_LINES;
  RB::invalidate();
  assert(!RB::active() && !RB::paint(r, box) && r.fills == 0);
  // On: drawn once, then copied while nothing changes; the B/W proxy stays paper.
  SETTINGS.readingBackground = S::READING_BACKGROUND_MEDIUM;
  assert(RB::active());
  r.bw.assign(r.bw.size(), 0);
  assert(RB::paint(r, box) && r.fills > 0 && heapAllocations == 1);
  for (const auto byte : r.bw) assert(byte == 0xFF);
  const auto drawn = r.packed;
  const int fills = r.fills;
  r.packed.assign(r.packed.size(), 0x00);
  assert(RB::paint(r, box) && r.fills == fills && r.packed == drawn);
  assert(!RB::paint(r, columns));  // ruled paper follows horizontal rows only
  // Another text box, shade, or an invalidate() draws again.
  RB::TextBox moved = box;
  moved.pitch = 60;
  assert(RB::paint(r, moved) && r.fills > fills && r.packed != drawn);
  const int movedFills = r.fills;
  RB::invalidate();
  assert(RB::paint(r, moved) && r.fills > movedFills && heapAllocations == 1);

  // The picture paper needs its file; it is cropped to the screen and lightened.
  SETTINGS.readingPaper = S::READING_PAPER_PICTURE;
  RB::invalidate();
  assert(!RB::active() && !RB::paint(r, box));
  Storage.files.insert(RB::kPath);
  RB::invalidate();
  assert(RB::active());
  r = GfxRenderer{};
  assert(RB::paint(r, columns) && r.pictures == 1);  // a picture needs no rows
  for (int y = 0; y < GfxRenderer::H; y += 97)
    for (int x = 0; x < GfxRenderer::W; x += 89) assert(r.level(x, y) == RB::lighten(0, S::READING_BACKGROUND_MEDIUM));
  assert(RB::paint(r, box) && r.pictures == 1);  // copied, not decoded again
  // A 16-level frame is required.
  GfxRenderer bwOnly;
  bwOnly.gray = false;
  assert(!RB::paint(bwOnly, box));
}
'''


class ReadingBackgroundTest(unittest.TestCase):
    def test_paper_follows_the_rows_and_the_drawn_background_is_reused(self):
        with tempfile.TemporaryDirectory(prefix='reading-background-') as folder:
            work = Path(folder)
            for name, text in STUBS.items():
                (work / name).write_text(text)
            (work / 'test.cpp').write_text(PROGRAM)
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(work), '-I', str(ROOT / 'src'), str(work / 'test.cpp'),
                            str(ROOT / 'src/util/ReadingBackground.cpp'), '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)

    def test_reader_lays_the_background_under_both_16_level_renders(self):
        reader = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        native = method(reader, 'bool EpubReaderActivity::nativeGray16Page(')
        self.assertIn('ReadingBackground::active()', native.split('#else', 1)[0])
        # The prerender into the page cache paints it right after the offscreen clear,
        # before panels and text, to this page's text box and body line height.
        cache = reader[reader.index('if (!renderer.beginGrayscale16Offscreen(pageCache16_[slot].get()))'):]
        cache = cache[:cache.index('page->renderPanels(')]
        self.assertIn('ReadingBackground::paint(renderer, box);', cache)
        self.assertIn('box.pitch = renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression);', cache)
        self.assertIn('readingGuideLine::rowOffset(box.pitch', cache)
        self.assertIn('if (!vertical) {', cache)
        # The foreground pass: after beginGrayscale16(), before the page; a cache hit
        # copies a page that already carries it.
        turn = reader[reader.index('    if (renderer.beginGrayscale16()) {'):]
        turn = turn[:turn.index('const auto t16Page = millis();')]
        self.assertLess(turn.index('pageCache16_[pageCacheLiveSlot_].get()'), turn.index('ReadingBackground::paint('))
        self.assertLess(turn.index('ReadingBackground::paint(renderer, box);'), turn.index('renderPageWithGuideLines();'))
        self.assertIn('box.pitch = renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression());', turn)
        self.assertEqual(reader.count('ReadingBackground::paint(renderer, box);'), 2)

    def test_settings_viewer_and_gallery_keep_the_background_in_step(self):
        page = (ROOT / 'src/activities/apps/standby/RickyStandbySettingsActivity.cpp').read_text()
        activate = method(page, 'void RickyStandbySettingsActivity::activateIndex(')
        self.assertEqual(activate.count('ReadingBackground::invalidate();'), 2)
        self.assertIn('openGallery(StandbyPictures::Target::Reading);', activate)
        viewer = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        self.assertIn('if (success && reading) ReadingBackground::invalidate();', viewer)
        pictures = (ROOT / 'src/activities/apps/standby/StandbyPictures.cpp').read_text()
        in_use = method(pictures, 'std::string sourceInUse(')
        self.assertIn('SETTINGS.rickyStandbyFace == CrossPointSettings::RICKY_STANDBY_PICTURE', in_use)
        self.assertIn('SETTINGS.rickyPowerOffScreen == CrossPointSettings::RICKY_POWER_OFF_CUSTOM', in_use)
        self.assertIn('SETTINGS.readingPaper == CrossPointSettings::READING_PAPER_PICTURE', in_use)
        gallery = (ROOT / 'src/activities/apps/standby/StandbyGalleryActivity.cpp').read_text()
        self.assertIn('inUse = StandbyPictures::sourceInUse(target);', gallery)
        self.assertIn('standbySource_ = StandbyPictures::sourceInUse(StandbyPictures::Target::Standby);', page)
        settings = (ROOT / 'src/CrossPointSettings.cpp').read_text()
        self.assertIn('if (readingBackground >= READING_BACKGROUND_COUNT) readingBackground = READING_BACKGROUND_OFF;',
                      settings)
        self.assertIn('if (readingPaper >= READING_PAPER_COUNT) readingPaper = READING_PAPER_PICTURE;', settings)


class ThumbnailAndPhotoTest(unittest.TestCase):
    def test_thumbnail_cache_is_listed_once_a_boot_and_keeps_its_newer_half(self):
        pictures = (ROOT / 'src/activities/apps/standby/StandbyPictures.cpp').read_text()
        prune = method(pictures, 'void pruneThumbs(')
        self.assertIn('static bool pruned = false;', prune)
        self.assertLess(prune.index('if (pruned) return;'), prune.index('Storage.open(kThumbDir)'))
        self.assertIn('const size_t drop = names.size() - kThumbCacheMax / 2;', prune)
        self.assertNotIn('for (const auto& entry : names) Storage.remove', prune)

    def test_gallery_shows_each_thumbnail_and_prefetches_the_next_page(self):
        gallery = (ROOT / 'src/activities/apps/standby/StandbyGalleryActivity.cpp').read_text()
        self.assertIn('void StandbyGalleryActivity::prefetchThumbnail()', gallery)
        loop = method(gallery, 'void StandbyGalleryActivity::loop()')
        self.assertIn('prefetchThumbnail();', loop)

    def test_large_photos_are_limited_by_their_decoded_size(self):
        jpeg = (ROOT / 'lib/JpegToBmpConverter/JpegToBmpConverter.cpp').read_text()
        self.assertIn('srcWidth > MAX_IMAGE_WIDTH * 8 || srcHeight > MAX_IMAGE_HEIGHT * 8', jpeg)
        self.assertIn('if (scaleSrcWidth > MAX_IMAGE_WIDTH || scaleSrcHeight > MAX_IMAGE_HEIGHT) {', jpeg)
        # A 3024x4032 phone photo fits once the decoder halves it.
        self.assertLessEqual((3024 + 1) // 2, 2048)
        self.assertLessEqual((4032 + 1) // 2, 3072)


if __name__ == '__main__':
    unittest.main()
