#include <gtest/gtest.h>

#include <map>
#include <utility>

#include "util/ReadingGuideLine.h"

namespace {

using readingGuideLine::Style;

constexpr uint8_t raw(const Style style) { return static_cast<uint8_t>(style); }

// A B/W renderer: records inked pixels.
struct InkRenderer {
  std::map<std::pair<int, int>, bool> pixels;
  void drawPixel(const int x, const int y, const bool state) { pixels[{x, y}] = state; }
  bool inked(const int x, const int y) const { return pixels.count({x, y}) != 0; }
};

// A 16-level frame: paper everywhere unless written; ink and gray recorded.
struct GrayRenderer {
  bool active = true;
  std::map<std::pair<int, int>, uint8_t> levels;  // 0..15
  int bwWrites = 0;
  bool isGrayscale16Active() const { return active; }
  uint8_t grayscale16Level(const int x, const int y) const {
    const auto it = levels.find({x, y});
    return it == levels.end() ? 15 : it->second;
  }
  void drawGrayscale16Pixel(const int x, const int y, const uint8_t gray) {
    levels[{x, y}] = static_cast<uint8_t>((gray + 8) / 17);
  }
  void drawPixel(int, int, bool) { ++bwWrites; }
};

TEST(ReadingGuideLine, StoredStylesKeepTheirNumbers) {
  EXPECT_EQ(raw(Style::Solid), 0);
  EXPECT_EQ(raw(Style::Wavy), 5);
  EXPECT_EQ(raw(Style::Double), 6);
  EXPECT_EQ(raw(Style::Highlight), 7);
  EXPECT_EQ(readingGuideLine::styleFor(200), Style::ShortDash);
}

TEST(ReadingGuideLine, DashRhythmsAreSizedForThePanel) {
  EXPECT_EQ(readingGuideLine::patternFor(raw(Style::Solid)), (readingGuideLine::Pattern{0, 0}));
  EXPECT_EQ(readingGuideLine::patternFor(raw(Style::ShortDash)), (readingGuideLine::Pattern{8, 6}));
  EXPECT_EQ(readingGuideLine::patternFor(raw(Style::MediumDash)), (readingGuideLine::Pattern{14, 8}));
  EXPECT_EQ(readingGuideLine::patternFor(raw(Style::LongDash)), (readingGuideLine::Pattern{24, 10}));
  EXPECT_EQ(readingGuideLine::patternFor(raw(Style::Dotted)), (readingGuideLine::Pattern{2, 4}));
  EXPECT_EQ(readingGuideLine::patternFor(255), (readingGuideLine::Pattern{8, 6}));
}

TEST(ReadingGuideLine, SolidIsATwoPixelStrokeWithNormalizedBounds) {
  InkRenderer renderer;
  readingGuideLine::draw(renderer, 12, 7, 3, raw(Style::Solid));
  EXPECT_EQ(renderer.pixels.size(), 20U);  // x 3..12, rows 7 and 8
  EXPECT_TRUE(renderer.inked(3, 7) && renderer.inked(12, 8));
  EXPECT_FALSE(renderer.inked(2, 7) || renderer.inked(13, 7) || renderer.inked(5, 9));
}

TEST(ReadingGuideLine, DashesClipTheFinalSegment) {
  InkRenderer renderer;
  readingGuideLine::draw(renderer, 0, 4, 17, raw(Style::ShortDash));
  for (int x = 0; x <= 17; ++x) EXPECT_EQ(renderer.inked(x, 4), x < 8 || x >= 14) << x;
}

TEST(ReadingGuideLine, WaveIsContinuousWithinItsExtent) {
  InkRenderer renderer;
  const int y = 20;
  readingGuideLine::draw(renderer, 0, y, 47, raw(Style::Wavy));
  const auto extent = readingGuideLine::extentFor(raw(Style::Wavy));
  for (int x = 0; x <= 47; ++x) {
    int count = 0;
    for (int row = y - extent.up; row <= y + extent.down; ++row) count += renderer.inked(x, row) ? 1 : 0;
    EXPECT_GE(count, readingGuideLine::kStroke) << x;
  }
  for (const auto& [point, state] : renderer.pixels) {
    EXPECT_GE(point.second, y - extent.up);
    EXPECT_LE(point.second, y + extent.down);
  }
}

TEST(ReadingGuideLine, DoubleLineLeavesPaperBetweenItsStrokes) {
  InkRenderer renderer;
  readingGuideLine::draw(renderer, 0, 10, 5, raw(Style::Double));
  EXPECT_TRUE(renderer.inked(0, 10) && renderer.inked(0, 11));
  EXPECT_FALSE(renderer.inked(0, 12) || renderer.inked(0, 13));
  EXPECT_TRUE(renderer.inked(0, 14) && renderer.inked(0, 15));
  EXPECT_EQ(readingGuideLine::extentFor(raw(Style::Double)), (readingGuideLine::Extent{0, 5}));
}

TEST(ReadingGuideLine, LinesAreGrayOnTheSixteenLevelFrameAndNeverLighten) {
  GrayRenderer renderer;
  renderer.levels[{2, 30}] = 0;  // a descender's ink pixel on the line
  readingGuideLine::draw(renderer, 0, 30, 5, raw(Style::Solid));
  EXPECT_EQ(renderer.bwWrites, 0);
  EXPECT_EQ(renderer.grayscale16Level(0, 30), 7);  // #777, ink in the B/W proxy
  EXPECT_EQ(renderer.grayscale16Level(2, 30), 0);
  EXPECT_LT(renderer.grayscale16Level(0, 30), 8);
}

TEST(ReadingGuideLine, HighlightIsATintBandOrDotsWithoutTheGrayFrame) {
  GrayRenderer gray;
  readingGuideLine::draw(gray, 0, 40, 9, raw(Style::Highlight));
  const auto extent = readingGuideLine::extentFor(raw(Style::Highlight));
  EXPECT_EQ(extent.up + extent.down + 1, readingGuideLine::kHighlightHeight);
  for (int row = 40 - extent.up; row <= 40 + extent.down; ++row) EXPECT_EQ(gray.grayscale16Level(4, row), 13);
  EXPECT_EQ(gray.grayscale16Level(4, 40 - extent.up - 1), 15);

  gray.active = false;  // night mode / B/W pass: the band must not become a black bar
  InkRenderer ink;
  readingGuideLine::draw(ink, 0, 40, 11, raw(Style::Highlight));
  EXPECT_TRUE(ink.inked(0, 40) && ink.inked(1, 40));
  EXPECT_FALSE(ink.inked(2, 40) || ink.inked(0, 39));
  EXPECT_TRUE(ink.inked(6, 41));
}

TEST(ReadingGuideLine, FitsUsesEachStylesExtent) {
  const auto wavy = raw(Style::Wavy);
  EXPECT_TRUE(readingGuideLine::fitsVertically(wavy, 2, 0, 6));
  EXPECT_FALSE(readingGuideLine::fitsVertically(wavy, 1, 0, 6));
  EXPECT_FALSE(readingGuideLine::fitsVertically(wavy, 3, 0, 6));
  EXPECT_TRUE(readingGuideLine::fitsVertically(raw(Style::Solid), 0, 0, 2));
  EXPECT_FALSE(readingGuideLine::fitsVertically(raw(Style::Solid), 1, 0, 2));
  EXPECT_FALSE(readingGuideLine::fitsVertically(raw(Style::Highlight), 7, 0, 100));
}

TEST(ReadingGuideLine, MenuChoicesRoundTrip) {
  EXPECT_EQ(readingGuideLine::menuIndexFor(false, raw(Style::Wavy)), 0);
  for (int i = 1; i < readingGuideLine::kMenuChoices; ++i) {
    EXPECT_EQ(readingGuideLine::menuIndexFor(true, raw(readingGuideLine::kMenuStyles[i])), i);
  }
  EXPECT_EQ(readingGuideLine::menuIndexFor(true, raw(Style::LongDash)), 2);
  EXPECT_EQ(readingGuideLine::menuIndexFor(true, 200), 2);
}

}  // namespace
