#include <gtest/gtest.h>

#include <string>

#include "TxtChapterIndex.h"

TEST(TxtChapterIndex, RecognizesChineseNovelHeadings) {
  using txt_chapter_index::chapterTitle;

  EXPECT_EQ(chapterTitle("第1章 开始"), "第1章 开始");
  EXPECT_EQ(chapterTitle("  第 十二 回 归来\r"), "第 十二 回 归来");
  EXPECT_EQ(chapterTitle("第二百三十四节"), "第二百三十四节");
  EXPECT_EQ(chapterTitle("第两万零一篇 尾声"), "第两万零一篇 尾声");
  EXPECT_TRUE(chapterTitle("第一天发生了很多事").empty());
  EXPECT_TRUE(chapterTitle("这是第1章的正文说明").empty());
  EXPECT_TRUE(chapterTitle("第章 缺少编号").empty());
}

TEST(TxtChapterIndex, RecognizesNumberedEnglishHeadings) {
  using txt_chapter_index::chapterTitle;

  EXPECT_EQ(chapterTitle("Chapter 1: Arrival"), "Chapter 1: Arrival");
  EXPECT_EQ(chapterTitle("chapter xii The Return"), "chapter xii The Return");
  EXPECT_EQ(chapterTitle("BOOK IV—Winter"), "BOOK IV—Winter");
  EXPECT_EQ(chapterTitle("Part 3. Home"), "Part 3. Home");
  EXPECT_TRUE(chapterTitle("Chapter One").empty());
  EXPECT_TRUE(chapterTitle("Chapterhouse 2").empty());
  EXPECT_TRUE(chapterTitle("A note about Chapter 2").empty());
}

TEST(TxtChapterIndex, RejectsEmptyAndOversizedLines) {
  EXPECT_TRUE(txt_chapter_index::chapterTitle(" \t\r").empty());
  EXPECT_EQ(txt_chapter_index::chapterTitle("\xEF\xBB\xBF"
                                            "Chapter 1"),
            "Chapter 1");

  std::string oversized = "Chapter 1 ";
  oversized.append(txt_chapter_index::TITLE_CAPACITY, 'x');
  EXPECT_TRUE(txt_chapter_index::chapterTitle(oversized).empty());
}

TEST(MarkdownChapterIndex, HeadingLevelsWhitespaceAndClosingHashes) {
  for (uint8_t expected = 1; expected <= 6; ++expected) {
    uint8_t level;
    EXPECT_EQ(txt_chapter_index::markdownTitle("  " + std::string(expected, '#') + "\t中文标题 ##\r", level),
              "中文标题");
    EXPECT_EQ(level, expected);
  }
  for (auto line : {"#hashtag", "####### title", "    # code", "\t# code", "#   ", "\\# escaped", "> # quote"}) {
    uint8_t level;
    EXPECT_TRUE(txt_chapter_index::markdownTitle(line, level).empty()) << line;
    EXPECT_EQ(level, 0);
  }
}

TEST(MarkdownChapterIndex, FencesRequireMatchingCharacterLengthAndClosingWhitespace) {
  txt_chapter_index::MarkdownHeadings headings;
  EXPECT_FALSE(headings.accept("   ```md"));
  EXPECT_FALSE(headings.accept("# code"));
  EXPECT_FALSE(headings.accept("~~~"));
  EXPECT_FALSE(headings.accept("``"));
  EXPECT_FALSE(headings.accept("``` trailing"));
  EXPECT_FALSE(headings.accept("## still code"));
  EXPECT_FALSE(headings.accept("   ```` \r"));
  EXPECT_TRUE(headings.accept("# visible"));
  EXPECT_FALSE(headings.accept("    ~~~"));
  EXPECT_TRUE(headings.accept("## still visible"));
}
