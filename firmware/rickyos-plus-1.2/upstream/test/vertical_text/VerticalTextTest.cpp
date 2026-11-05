#include <gtest/gtest.h>

#include "Epub/VerticalText.h"

using verticalText::Placement;

TEST(VerticalText, IdeographsAndFullwidthFormsStandUpright) {
  EXPECT_EQ(verticalText::placementFor(0x4E2D), Placement::Upright);  // 中
  EXPECT_EQ(verticalText::placementFor(0x3042), Placement::Upright);  // あ
  EXPECT_EQ(verticalText::placementFor(0xFF01), Placement::Upright);  // ！
  EXPECT_EQ(verticalText::placementFor(0xFF1F), Placement::Upright);  // ？
  EXPECT_EQ(verticalText::placementFor(0xFF11), Placement::Upright);  // １
}

TEST(VerticalText, CommasAndStopsMoveToTheTopRight) {
  EXPECT_EQ(verticalText::placementFor(0x3001), Placement::CornerTopRight);  // 、
  EXPECT_EQ(verticalText::placementFor(0x3002), Placement::CornerTopRight);  // 。
  EXPECT_EQ(verticalText::placementFor(0xFF0C), Placement::CornerTopRight);  // ，
  EXPECT_EQ(verticalText::placementFor(0xFF0E), Placement::CornerTopRight);  // ．
}

TEST(VerticalText, BracketsDashesAndLatinTurn) {
  for (const uint32_t cp : {0x300CU, 0x300DU, 0x300EU, 0x300FU, 0x300AU, 0x300BU, 0xFF08U, 0xFF09U, 0x3010U, 0x3011U,
                            0xFF1AU, 0xFF1BU, 0xFF5EU}) {
    EXPECT_EQ(verticalText::placementFor(cp), Placement::Rotated) << std::hex << cp;
  }
  EXPECT_EQ(verticalText::placementFor('A'), Placement::Rotated);
  EXPECT_EQ(verticalText::placementFor('7'), Placement::Rotated);
  EXPECT_EQ(verticalText::placementFor(0x2014), Placement::Rotated);  // —
  EXPECT_EQ(verticalText::placementFor(0x2026), Placement::Rotated);  // …
}

TEST(VerticalText, TheTurnedFrameIsAQuarterTurnOn) {
  // Portrait -> LandscapeCounterClockwise, and every orientation stays in range.
  EXPECT_EQ(verticalText::turnedOrientation(0), 3);
  EXPECT_EQ(verticalText::turnedOrientation(1), 0);
  EXPECT_EQ(verticalText::turnedOrientation(2), 1);
  EXPECT_EQ(verticalText::turnedOrientation(3), 2);
}

TEST(VerticalText, TurnedCoordinatesRunDownTheColumn) {
  constexpr int kWidth = 684;
  // The top-right corner is the turned frame's origin row.
  constexpr auto corner = verticalText::toTurned(kWidth - 1, 0, kWidth);
  static_assert(corner.x == 0 && corner.y == 0);
  // Moving down the page moves along the turned x axis; moving left moves along turned y.
  constexpr auto lower = verticalText::toTurned(kWidth - 1, 50, kWidth);
  static_assert(lower.x == 50 && lower.y == 0);
  constexpr auto left = verticalText::toTurned(kWidth - 101, 0, kWidth);
  static_assert(left.x == 0 && left.y == 100);
  SUCCEED();
}
