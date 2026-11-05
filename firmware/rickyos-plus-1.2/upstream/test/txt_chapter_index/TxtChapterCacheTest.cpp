#include <HalStorage.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "Txt.h"

namespace {

class TxtChapterCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> serial{0};
    root_ = std::filesystem::temp_directory_path() /
            ("crossmux-txt-chapters-" + std::to_string(getpid()) + "-" + std::to_string(serial++));
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    ASSERT_TRUE(std::filesystem::create_directories(root_, error));
    ASSERT_FALSE(error);
    ASSERT_EQ(setenv("CROSSPOINT_SIM_SD", root_.c_str(), 1), 0);
    ASSERT_TRUE(Storage.begin());
  }

  void TearDown() override {
    unsetenv("CROSSPOINT_SIM_SD");
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::filesystem::path hostPath(std::string path) const {
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    return root_ / path;
  }

  void writeBook(const std::string& contents) {
    std::ofstream output(hostPath("/book.txt"), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output.good());
  }

  std::filesystem::path root_;
};

TEST_F(TxtChapterCacheTest, BuildsReusesAndInvalidatesTheFixedRecordCache) {
  const std::string contents = "Preface\nChapter 1: Start\nbody\nPart II - End\n";
  writeBook(contents);

  Txt txt("/book.txt", "/.crosspoint");
  ASSERT_TRUE(txt.load());
  std::array<uint8_t, 8193> scratch{};
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t count = 0;
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  ASSERT_EQ(count, 2U);

  HalFile cache;
  encoding = txt_encoding::Encoding::Unknown;
  ASSERT_TRUE(txt.openChapterIndex(cache, encoding, count));
  EXPECT_EQ(encoding, txt_encoding::Encoding::Unknown);
  txt_chapter_index::Record first;
  ASSERT_TRUE(txt.readChapter(cache, count, 0, first));
  EXPECT_STREQ(first.title, "Chapter 1: Start");
  EXPECT_EQ(first.sourceOffset, contents.find("Chapter 1"));
  cache.close();

  const auto cachePath = hostPath(txt.getCachePath() + "/chapters.bin");
  ASSERT_TRUE(std::filesystem::exists(cachePath));
  std::filesystem::resize_file(cachePath, 17);
  HalFile truncated;
  EXPECT_FALSE(txt.openChapterIndex(truncated, encoding, count));
  truncated.close();

  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  {
    std::fstream damaged(cachePath, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(damaged.good());
    damaged.seekp(20);  // Header plus the first record's source offset.
    damaged.put('x');
  }
  HalFile damaged;
  EXPECT_FALSE(txt.openChapterIndex(damaged, encoding, count));
  damaged.close();

  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  writeBook(contents + "Chapter 3\n");
  Txt changed("/book.txt", "/.crosspoint");
  ASSERT_TRUE(changed.load());
  HalFile stale;
  EXPECT_FALSE(changed.openChapterIndex(stale, encoding, count));
}

TEST_F(TxtChapterCacheTest, CachesAnEmptyChapterList) {
  writeBook("plain text without headings\n");
  Txt txt("/book.txt", "/.crosspoint");
  ASSERT_TRUE(txt.load());
  std::array<uint8_t, 8193> scratch{};
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t count = 99;
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  EXPECT_EQ(count, 0U);

  HalFile cache;
  ASSERT_TRUE(txt.openChapterIndex(cache, encoding, count));
  EXPECT_EQ(count, 0U);
}

TEST_F(TxtChapterCacheTest, DetectsAndCachesGbkChapterTitles) {
  const std::string gbkBook = "ASCII preface\n\xB5\xDA\xD2\xBB\xD5\xC2 \xBF\xAA\xCA\xBC\n";
  writeBook(gbkBook);
  Txt txt("/book.txt", "/.crosspoint");
  ASSERT_TRUE(txt.load());

  std::array<uint8_t, 8193> scratch{};
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t count = 0;
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  ASSERT_EQ(encoding, txt_encoding::Encoding::Gbk);
  ASSERT_EQ(count, 1U);

  HalFile cache;
  ASSERT_TRUE(txt.openChapterIndex(cache, encoding, count));
  txt_chapter_index::Record chapter;
  ASSERT_TRUE(txt.readChapter(cache, count, 0, chapter));
  EXPECT_STREQ(chapter.title, "第一章 开始");
  EXPECT_EQ(chapter.sourceOffset, gbkBook.find("\xB5\xDA"));
}

}  // namespace

#ifdef RICKYOS_PRODUCT
TEST_F(TxtChapterCacheTest, MarkdownIndexesAtxOutsideFencesAndRejectsOldCache) {
  const std::string source =
      "\xEF\xBB\xBF# 标题\r\n正文\n##\t**小节** ##\n```cpp\n# 假标题\n````\n"
      "    # 缩进代码\n~~~~\n## 另一假标题\n~~~\n~~~~\n###### 最深\n#hashtag\n";
  writeBook(source);
  std::filesystem::rename(hostPath("/book.txt"), hostPath("/book.MD"));
  Txt txt("/book.MD", "/.crosspoint");
  ASSERT_TRUE(txt.load());
  std::array<uint8_t, 8192> scratch{};
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t count = 0;
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  ASSERT_EQ(count, 3U);
  HalFile cache;
  ASSERT_TRUE(txt.openChapterIndex(cache, encoding, count));
  for (uint32_t i = 0; i < count; ++i) {
    txt_chapter_index::Record chapter;
    ASSERT_TRUE(txt.readChapter(cache, count, i, chapter));
    uint8_t level = 0;
    ASSERT_FALSE(txt_chapter_index::markdownTitle(chapter.title, level).empty());
    EXPECT_EQ(level, i == 0 ? 1 : i == 1 ? 2 : 6);
    EXPECT_EQ(chapter.sourceOffset, i == 0 ? 0U : source.find(i == 1 ? "##\t" : "######"));
  }
  cache.close();
  {
    std::fstream old(hostPath(txt.getCachePath() + "/chapters.bin"), std::ios::binary | std::ios::in | std::ios::out);
    old.seekp(14);
    old.put(1);
  }
  HalFile old;
  EXPECT_FALSE(txt.openChapterIndex(old, encoding, count));
  old.close();
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  HalFile rebuilt;
  EXPECT_TRUE(txt.openChapterIndex(rebuilt, encoding, count));
  EXPECT_EQ(count, 3U);
}

TEST_F(TxtChapterCacheTest, MarkdownLongFenceInfoDoesNotLeakFakeHeadings) {
  writeBook("```" + std::string(500, 'x') + "\n# hidden\n```\n## visible\n" + std::string(5000, 'z') + "\n### last");
  std::filesystem::rename(hostPath("/book.txt"), hostPath("/book.md"));
  Txt txt("/book.md", "/.crosspoint");
  ASSERT_TRUE(txt.load());
  std::array<uint8_t, 8192> scratch{};
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t count = 0;
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  EXPECT_EQ(count, 2U);
}
#endif

TEST_F(TxtChapterCacheTest, Utf16EndianChapterOffsetsAndChunkCarry) {
  for (bool big : {false, true}) {
    std::u16string text = u"第一章 起点\r\n正文\u0a41\n";
    text += std::u16string(244, u'x');
    text += u"\U0001f431\n第二章 终点\n尾";
    std::string bytes;
    bytes.reserve(2 + text.size() * 2);
    bytes += big ? std::string("\xfe\xff", 2) : std::string("\xff\xfe", 2);
    for (char16_t c : text) {
      bytes.push_back(big ? c >> 8 : c & 255);
      bytes.push_back(big ? c & 255 : c >> 8);
    }
    writeBook(bytes);
    Txt txt("/book.txt", "/.crosspoint");
    ASSERT_TRUE(txt.load());
    // Odd read capacity forces code units and surrogate pairs across reads.
    std::array<uint8_t, 1027> scratch{};
    uint32_t count = 0;
    auto encoding = txt_encoding::Encoding::Unknown;
    txt_encoding::ReadError error = txt_encoding::ReadError::Io;
    ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count, &error));
    EXPECT_EQ(error, txt_encoding::ReadError::None);
    EXPECT_EQ(count, 2U);
    EXPECT_EQ(encoding, big ? txt_encoding::Encoding::Utf16Be : txt_encoding::Encoding::Utf16Le);
    HalFile index;
    ASSERT_TRUE(txt.openChapterIndex(index, encoding, count));
    txt_chapter_index::Record record;
    ASSERT_TRUE(txt.readChapter(index, count, 0, record));
    EXPECT_STREQ(record.title, "第一章 起点");
    EXPECT_EQ(record.sourceOffset, 2U);
    ASSERT_TRUE(txt.readChapter(index, count, 1, record));
    EXPECT_STREQ(record.title, "第二章 终点");
    EXPECT_EQ(record.sourceOffset, 2 + text.find(u"第二章") * 2);
  }
}
TEST_F(TxtChapterCacheTest, Utf16MalformedInputIsNotAnIoErrorOrCommittedIndex) {
  for (const auto& bytes : {std::string("\xff\xfe\x00\xd8", 4), std::string("\xfe\xff\x41", 3),
                            std::string("\xff\xfe\x00\x00", 4), std::string("\x00\x00\xfe\xff", 4)}) {
    writeBook(bytes);
    Txt txt("/book.txt", "/.crosspoint");
    ASSERT_TRUE(txt.load());
    std::array<uint8_t, 1027> scratch{};
    uint32_t count = 0;
    auto encoding = txt_encoding::Encoding::Unknown;
    txt_encoding::ReadError error = txt_encoding::ReadError::None;
    EXPECT_FALSE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count, &error));
    EXPECT_EQ(error, txt_encoding::ReadError::InvalidData);
    EXPECT_EQ(count, 0U);
    EXPECT_FALSE(std::filesystem::exists(hostPath(txt.getCachePath() + "/chapters.bin")));
  }
}
TEST_F(TxtChapterCacheTest, UserUtf16SampleBuildsDirectoryWithoutLoadingWholeBook) {
  const char* sample = std::getenv("RICKYOS_UTF16_SAMPLE");
  if (!sample) GTEST_SKIP() << "optional local sample";
  std::filesystem::copy_file(sample, hostPath("/book.txt"), std::filesystem::copy_options::overwrite_existing);
  Txt txt("/book.txt", "/.crosspoint");
  ASSERT_TRUE(txt.load());
  std::array<uint8_t, 8193> scratch{};
  uint32_t count = 0;
  auto encoding = txt_encoding::Encoding::Unknown;
  ASSERT_TRUE(txt.buildChapterIndex(encoding, scratch.data(), scratch.size(), count));
  EXPECT_EQ(encoding, txt_encoding::Encoding::Utf16Le);
  EXPECT_GT(count, 0U);
  HalFile index;
  ASSERT_TRUE(txt.openChapterIndex(index, encoding, count));
  txt_chapter_index::Record last;
  ASSERT_TRUE(txt.readChapter(index, count, count - 1, last));
  EXPECT_LT(last.sourceOffset, txt.getFileSize());
  EXPECT_EQ(last.sourceOffset % 2, 0U);
}
