#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "activities/reader/ReadingMode.h"
#include "components/icons/rickyReaderDog.h"

using readingMode::Box;
using readingMode::Margins;
using readingMode::Mode;

namespace {
// Read Pico portrait: 684 x 1216, no panel inset, the default 25 px screen margin,
// and the 96 px reader footer below the text (no header: the puppy takes its place).
constexpr int kWidth = 684;
constexpr int kHeight = 1216;
constexpr Margins kViewable{0, 0, 0, 0};
constexpr Margins kBase{25, 25, 96, 25};

std::string numeral(const int value) {
  char out[32];
  readingMode::chineseNumeral(value, out, sizeof(out));
  return out;
}

bool inside(const Box& inner, const Box& outer) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}
}  // namespace

TEST(ReadingMode, UnknownValuesFallBackToOriginal) {
  EXPECT_EQ(readingMode::modeFor(0), Mode::Original);
  EXPECT_EQ(readingMode::modeFor(3), Mode::Ancient);
  EXPECT_EQ(readingMode::modeFor(4), Mode::Original);
  EXPECT_EQ(readingMode::modeFor(255), Mode::Original);
}

TEST(ReadingMode, ChromeBandsPerMode) {
  EXPECT_TRUE(readingMode::showsHeader(Mode::Original));
  EXPECT_FALSE(readingMode::showsHeader(Mode::Puppy));
  EXPECT_FALSE(readingMode::showsHeader(Mode::Immersive));
  EXPECT_FALSE(readingMode::showsHeader(Mode::Ancient));
  EXPECT_TRUE(readingMode::showsFooter(Mode::Original));
  EXPECT_TRUE(readingMode::showsFooter(Mode::Puppy));
  EXPECT_FALSE(readingMode::showsFooter(Mode::Immersive));
  EXPECT_FALSE(readingMode::showsFooter(Mode::Ancient));
}

TEST(ReadingMode, OriginalAndImmersiveUseTheBaseMargins) {
  // The caller drops the header and footer bands for Immersive; the mode adds nothing.
  EXPECT_EQ(readingMode::textMargins(Mode::Original, kBase, kViewable, 92), kBase);
  EXPECT_EQ(readingMode::textMargins(Mode::Immersive, kBase, kViewable, 92), kBase);
}

TEST(ReadingMode, PuppyFrameAndDogFitOnTheScreen) {
  const int edge = RickyReaderDog::kEdgeRow;
  const Margins m = readingMode::textMargins(Mode::Puppy, kBase, kViewable, edge);
  const Box text = readingMode::textBox(kWidth, kHeight, m);
  const Box frame = readingMode::puppyFrame(text);
  EXPECT_TRUE(inside(frame, Box{0, 0, kWidth, kHeight}));
  EXPECT_GE(frame.x, readingMode::kScreenGap);
  EXPECT_GE(kWidth - frame.right(), readingMode::kScreenGap);
  // The dog's head clears the top of the screen and its paws stay out of the text.
  const int dogTop = frame.y + readingMode::kHairline - 1 - edge;
  EXPECT_GE(dogTop, 0);
  EXPECT_LE(dogTop + RickyReaderDog::kHeight, text.y);
  // The frame closes above the footer band.
  EXPECT_LE(frame.bottom(), kHeight - kBase.bottom);
  // The text column keeps the original width at the default margin.
  EXPECT_EQ(text.width, kWidth - kBase.left - kBase.right);
}

TEST(ReadingMode, PuppyKeepsAWiderMarginTheReaderChose) {
  const Margins wide{40, 40, 80, 40};
  const Margins m = readingMode::textMargins(Mode::Puppy, wide, kViewable, RickyReaderDog::kEdgeRow);
  EXPECT_EQ(m.left, 40);
  EXPECT_EQ(m.right, 40);
}

TEST(ReadingMode, AncientFrameHoldsTheTextAndTheBanxinColumn) {
  const Margins base{25, 25, 25, 25};  // no status bar reserve in this mode
  const Margins m = readingMode::textMargins(Mode::Ancient, base, kViewable, 0);
  const Box text = readingMode::textBox(kWidth, kHeight, m);
  const Box frame = readingMode::ancientFrame(text);
  const Box column = readingMode::banxin(text);
  EXPECT_TRUE(inside(frame, Box{0, 0, kWidth, kHeight}));
  EXPECT_GE(frame.x, readingMode::kScreenGap);
  EXPECT_GE(kWidth - frame.right(), readingMode::kScreenGap);
  EXPECT_TRUE(inside(column, frame));
  EXPECT_EQ(column.width, readingMode::kBanxinWidth);
  // Columns run right to left: the first starts at the frame's inner rule, and the
  // 版心 closes the page on the left, its own rule between it and the text.
  EXPECT_EQ(text.right(), frame.right() - readingMode::kAncientRules);
  EXPECT_EQ(column.x, frame.x + readingMode::kAncientRules);
  EXPECT_EQ(column.right() + readingMode::kHairline, text.x);
  // 天头 is taller than 地脚.
  EXPECT_GT(frame.y, kHeight - frame.bottom());
}

TEST(ReadingMode, AncientHoldsWholeColumnsAndCentresTheFrame) {
  EXPECT_FALSE(readingMode::setsVertically(Mode::Original));
  EXPECT_TRUE(readingMode::setsVertically(Mode::Ancient));
  const Margins base{25, 25, 25, 25};
  const Margins raw = readingMode::textMargins(Mode::Ancient, base, kViewable, 0);
  for (const int pitch : {37, 48, 53, 61}) {
    const Margins m = readingMode::fitColumns(raw, kViewable, kWidth, pitch);
    const Box text = readingMode::textBox(kWidth, kHeight, m);
    const Box frame = readingMode::ancientFrame(text);
    EXPECT_EQ(text.width % pitch, 0) << pitch;
    // Only the partial column is given up.
    EXPECT_GT(text.width + pitch, kWidth - raw.left - raw.right) << pitch;
    const int leftGap = frame.x;
    const int rightGap = kWidth - frame.right();
    EXPECT_LE(std::abs(leftGap - rightGap), 1) << pitch;
    EXPECT_GE(std::min(leftGap, rightGap), readingMode::kScreenGap) << pitch;
    EXPECT_EQ(m.top, raw.top);
    EXPECT_EQ(m.bottom, raw.bottom);
  }
  // A pitch wider than the text leaves the margins alone.
  EXPECT_EQ(readingMode::fitColumns(raw, kViewable, kWidth, 2000), raw);
}

TEST(ReadingMode, ChineseNumerals) {
  EXPECT_EQ(numeral(1), "一");
  EXPECT_EQ(numeral(9), "九");
  EXPECT_EQ(numeral(10), "十");
  EXPECT_EQ(numeral(12), "十二");
  EXPECT_EQ(numeral(20), "二十");
  EXPECT_EQ(numeral(99), "九十九");
  EXPECT_EQ(numeral(100), "一百");
  EXPECT_EQ(numeral(101), "一百零一");
  EXPECT_EQ(numeral(110), "一百一十");
  EXPECT_EQ(numeral(206), "二百零六");
  EXPECT_EQ(numeral(1000), "一千");
  EXPECT_EQ(numeral(1001), "一千零一");
  EXPECT_EQ(numeral(1010), "一千零一十");
  EXPECT_EQ(numeral(1100), "一千一百");
  EXPECT_EQ(numeral(9999), "九千九百九十九");
  EXPECT_EQ(numeral(0), "零");
}

TEST(ReadingMode, ChineseNumeralsFallBackToDigits) {
  EXPECT_EQ(numeral(10000), "10000");
  EXPECT_EQ(numeral(-3), "0");
  char tiny[4];  // room for one CJK character only
  readingMode::chineseNumeral(12, tiny, sizeof(tiny));
  EXPECT_STREQ(tiny, "12");
}
