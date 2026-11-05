#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "Print.h"
#include "TxtToHtml.h"

namespace {

class StringPrint : public Print {
 public:
  std::string str;
  size_t write(uint8_t b) override {
    str.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    str.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

const std::string kHeader =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- TXT_CACHE_VERSION: 3 -->\n"
    "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\n";
const std::string kFooter = "\n</body>\n</html>\n";

std::string convert(std::string_view content) {
  StringPrint out;
  EXPECT_TRUE(TxtToHtml::stream("test.txt", content, out));
  return out.str;
}

TEST(TxtToHtmlTest, LeadingSpacesIndentation) {
  EXPECT_EQ(convert("  two spaces"), kHeader + "&#160;&#160;two spaces" + kFooter);
  EXPECT_EQ(convert("    four spaces"), kHeader + "&#160;&#160;&#160;&#160;four spaces" + kFooter);
  EXPECT_EQ(convert("Line 1\n   three spaces"), kHeader + "Line 1<br />&#160;&#160;&#160;three spaces" + kFooter);
}

TEST(TxtToHtmlTest, MidLineConsecutiveSpaces) {
  EXPECT_EQ(convert("One space"), kHeader + "One space" + kFooter);
  EXPECT_EQ(convert("Two  spaces"), kHeader + "Two&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Three   spaces"), kHeader + "Three&#160;&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Four    spaces"), kHeader + "Four&#160;&#160;&#160; spaces" + kFooter);
}

TEST(TxtToHtmlTest, PreservesCacheVersionTagsForBothFormats) {
  StringPrint out;
  ASSERT_TRUE(TxtToHtml::stream("test.MD", "text", out));
  EXPECT_EQ(out.str,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- MD_CACHE_VERSION: 6 -->\n"
            "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\n<p>text</p>" +
                kFooter);
  EXPECT_NE(out.str.find(TxtToHtml::cacheVersionTag("test.MD")), std::string::npos);
  EXPECT_NE(convert("text").find(TxtToHtml::cacheVersionTag("test.txt")), std::string::npos);
}

}  // namespace

namespace {
struct ChunkReader {
  std::string_view text;
  size_t pos = 0;
  size_t chunk = 1;
  static int read(void* context, uint8_t* data, size_t capacity) {
    auto& reader = *static_cast<ChunkReader*>(context);
    const size_t size = std::min({capacity, reader.chunk, reader.text.size() - reader.pos});
    memcpy(data, reader.text.data() + reader.pos, size);
    reader.pos += size;
    return static_cast<int>(size);
  }
};

TEST(TxtToHtmlStream, EncodingAcrossSingleByteAndBlockBoundaries) {
  for (const size_t chunk : {1U, 7U, 8192U}) {
    const std::string utf8 = std::string(8191, 'a') + "中文\n<&>";
    ChunkReader reader{utf8, 0, chunk};
    StringPrint out;
    ASSERT_TRUE(TxtToHtml::stream("test.txt", &reader, ChunkReader::read, out));
    EXPECT_NE(out.str.find(std::string(8191, 'a') + "中文<br />&lt;&amp;&gt;"), std::string::npos);
    const std::string gbk = std::string(8191, 'a') + "\xD6\xD0\xCE\xC4\r\n";
    ChunkReader gbkReader{gbk, 0, chunk};
    StringPrint gbkOut;
    TxtToHtml::Options options;
    options.encoding = txt_encoding::Encoding::Gbk;
    ASSERT_TRUE(TxtToHtml::stream("test.txt", &gbkReader, ChunkReader::read, gbkOut, options));
    EXPECT_NE(gbkOut.str.find(std::string(8191, 'a') + "中文<br />"), std::string::npos);
  }
}

TEST(TxtToHtmlStream, RejectsTruncationReadAndWriteFailures) {
  StringPrint invalid;
  EXPECT_FALSE(TxtToHtml::stream("test.txt", "\xE4\xB8", invalid));
  StringPrint failed;
  EXPECT_FALSE(TxtToHtml::stream("test.txt", nullptr, [](void*, uint8_t*, size_t) { return -1; }, failed));
  class FullPrint : public Print {
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
  } full;
  EXPECT_FALSE(TxtToHtml::stream("test.txt", "hello", full));
}

TEST(TxtToHtmlStream, ConvertsLargeLongLineWithoutHoldingBookOrChapters) {
  struct LargeReader {
    size_t remaining = 32 * 1024 * 1024;
    size_t maximumRequest = 0;
  } reader;
  class CountingPrint : public Print {
   public:
    size_t count = 0;
    size_t write(uint8_t) override {
      ++count;
      return 1;
    }
    size_t write(const uint8_t*, size_t size) override {
      count += size;
      return size;
    }
  } out;
  ASSERT_TRUE(TxtToHtml::stream(
      "test.txt", &reader,
      [](void* context, uint8_t* data, size_t capacity) {
        auto& reader = *static_cast<LargeReader*>(context);
        reader.maximumRequest = std::max(reader.maximumRequest, capacity);
        const size_t size = std::min(reader.remaining, capacity);
        memset(data, 'a', size);
        reader.remaining -= size;
        return static_cast<int>(size);
      },
      out));
  EXPECT_EQ(reader.maximumRequest, 8192U);
  EXPECT_EQ(out.count, 32 * 1024 * 1024 + kHeader.size() + kFooter.size());
}

TEST(TxtToHtmlStream, EmitsSourceMappingAndChapterAnchorsInSourceOrder) {
  struct Context {
    unsigned chapter = 0;
    uint32_t lastSource = 0;
    uint32_t lastVisible = 0;
    bool first = true;
  } context;
  const std::string book = "intro  \r\nChapter 1\n中文 end";
  ChunkReader reader{book};
  StringPrint out;
  TxtToHtml::Options options;
  options.context = &context;
  options.chapterOffset = [](void* p) { return static_cast<Context*>(p)->chapter == 0 ? 9U : UINT32_MAX; };
  options.nextChapter = [](void* p) {
    ++static_cast<Context*>(p)->chapter;
    return true;
  };
  options.map = [](void* p, uint32_t source, uint32_t visible, uint8_t, uint8_t) {
    auto& context = *static_cast<Context*>(p);
    if (!context.first) {
      EXPECT_GE(source, context.lastSource);
      EXPECT_GE(visible, context.lastVisible);
    }
    context.first = false;
    context.lastSource = source;
    context.lastVisible = visible;
    return true;
  };
  ASSERT_TRUE(TxtToHtml::stream("test.txt", &reader, ChunkReader::read, out, options));
  EXPECT_NE(out.str.find("<div id=\"txt-9\"></div>Chapter 1"), std::string::npos);
  EXPECT_EQ(context.chapter, 1U);
  EXPECT_EQ(context.lastSource, book.size());
  EXPECT_EQ(context.lastVisible, 1U + 5U + 9U + 2U + 4U);
}
}  // namespace

namespace {
std::string utf16Bytes(std::u16string_view text, bool big) {
  std::string out;
  out.reserve(2 + text.size() * 2);
  out += big ? std::string("\xfe\xff", 2) : std::string("\xff\xfe", 2);
  for (auto c : text) {
    out.push_back(big ? c >> 8 : c & 255);
    out.push_back(big ? c & 255 : c >> 8);
  }
  return out;
}
}  // namespace
TEST(TxtToHtmlUtf16, LeBeSingleByteChunksAndRawCoordinates) {
  const auto text = u"第一章\r\n  中文\U0001f431\u0a41<&>  空 格\n结尾";
  StringPrint expected;
  // Build the expected UTF-8 from exact scalars rather than locale conversion.
  const std::string utf8 = "第一章\r\n  中文\xf0\x9f\x90\xb1\xe0\xa9\x81<&>  空 格\n结尾";
  expected.str.clear();
  ASSERT_TRUE(TxtToHtml::stream("test.txt", utf8, expected));
  for (bool big : {false, true})
    for (size_t chunk : {1U, 3U, 7U, 8191U}) {
      const auto bytes = utf16Bytes(text, big);
      ChunkReader reader{bytes, 0, chunk};
      StringPrint out;
      txt_encoding::ReadError error = txt_encoding::ReadError::Io;
      TxtToHtml::Options opts;
      opts.encoding = big ? txt_encoding::Encoding::Utf16Be : txt_encoding::Encoding::Utf16Le;
      opts.error = &error;
      struct Mapping {
        uint32_t last = 0, end = 0;
        int surrogate = 0;
      };
      Mapping map;
      opts.context = &map;
      opts.map = [](void* opaque, uint32_t source, uint32_t, uint8_t width, uint8_t) {
        auto& m = *static_cast<Mapping*>(opaque);
        EXPECT_EQ(source % 2, 0U);
        EXPECT_GE(source, m.last);
        m.last = source;
        EXPECT_TRUE(width == 0 || width == 2 || width == 4);
        if (width == 4) m.surrogate++;
        if (!width) m.end = source;
        return true;
      };
      ASSERT_TRUE(TxtToHtml::stream("test.txt", &reader, ChunkReader::read, out, opts));
      EXPECT_EQ(out.str, expected.str);
      EXPECT_EQ(error, txt_encoding::ReadError::None);
      EXPECT_EQ(map.end, bytes.size());
      EXPECT_EQ(map.surrogate, 1);
    }
}
TEST(TxtToHtmlUtf16, RejectsOddBytesAndBadSurrogatesAsDataNotIo) {
  for (const auto& bytes : {std::string("\xff\xfe\x41", 3), std::string("\xff\xfe\x00\xd8", 4),
                            std::string("\xff\xfe\x00\xdc", 4), std::string("\xff\xfe\x00\xd8\x41\x00", 6)}) {
    ChunkReader reader{bytes, 0, 1};
    StringPrint out;
    TxtToHtml::Options opts;
    txt_encoding::ReadError error = txt_encoding::ReadError::None;
    opts.error = &error;
    opts.encoding = txt_encoding::Encoding::Utf16Le;
    EXPECT_FALSE(TxtToHtml::stream("test.txt", &reader, ChunkReader::read, out, opts));
    EXPECT_EQ(error, txt_encoding::ReadError::InvalidData);
  }
  StringPrint out;
  TxtToHtml::Options opts;
  txt_encoding::ReadError error = txt_encoding::ReadError::None;
  opts.error = &error;
  opts.encoding = txt_encoding::Encoding::Utf16Le;
  EXPECT_FALSE(TxtToHtml::stream("test.txt", nullptr, [](void*, uint8_t*, size_t) { return -1; }, out, opts));
  EXPECT_EQ(error, txt_encoding::ReadError::Io);
}

TEST(TxtToHtmlUtf16, UserBookMatchesIndependentUtf8ReferenceInBoundedReads) {
  const char* sample = std::getenv("RICKYOS_UTF16_SAMPLE");
  const char* reference = std::getenv("RICKYOS_UTF8_REFERENCE");
  if (!sample || !reference) GTEST_SKIP() << "optional local user book and independent UTF-8 reference";
  std::ifstream a(sample, std::ios::binary), b(reference, std::ios::binary);
  ASSERT_TRUE(a && b);
  const std::string raw((std::istreambuf_iterator<char>(a)), {}), utf8((std::istreambuf_iterator<char>(b)), {});
  ChunkReader reader{raw, 0, 4093};
  StringPrint actual, expected;
  TxtToHtml::Options opts;
  opts.encoding = txt_encoding::Encoding::Utf16Le;
  ASSERT_TRUE(TxtToHtml::stream("test.txt", &reader, ChunkReader::read, actual, opts));
  ASSERT_TRUE(TxtToHtml::stream("test.txt", utf8, expected));
  EXPECT_EQ(actual.str, expected.str);
  EXPECT_EQ(reader.pos, raw.size());
}
