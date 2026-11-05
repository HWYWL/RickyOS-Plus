#include <HalStorage.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "Txt.h"
namespace {
class Sink : public Print {
 public:
  std::string text;
  size_t write(uint8_t b) override {
    text.push_back(b);
    return 1;
  }
  size_t write(const uint8_t* b, size_t n) override {
    text.append(reinterpret_cast<const char*>(b), n);
    return n;
  }
};
class Pipeline : public ::testing::Test {
 protected:
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("rickyos-utf16-pipeline-" + std::to_string(getpid()));
  void SetUp() override {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "cache");
    setenv("CROSSPOINT_SIM_SD", root.c_str(), 1);
    ASSERT_TRUE(Storage.begin());
  }
  void TearDown() override {
    unsetenv("CROSSPOINT_SIM_SD");
    std::filesystem::remove_all(root);
  }
  void put(const std::string& b) {
    std::ofstream f(root / "book.txt", std::ios::binary);
    f.write(b.data(), b.size());
  }
};
TEST_F(Pipeline, ConvertsIndexesMapsAndReopensTheSameOriginalBook) {
  for (bool big : {false, true}) {
    const std::u16string text = u"第一章 初见\r\n正文\U0001f431\n第二章 后续\n再读";
    std::string raw;
    raw += big ? std::string("\xfe\xff", 2) : std::string("\xff\xfe", 2);
    for (char16_t c : text) {
      raw.push_back(big ? c >> 8 : c & 255);
      raw.push_back(big ? c & 255 : c >> 8);
    }
    put(raw);
    // Separate caches for the two encodings; test direct warm reuse below.
    std::filesystem::remove_all(root / ".crosspoint");
    Sink out;
    txt_encoding::ReadError error = txt_encoding::ReadError::Io;
    ASSERT_TRUE(Txt::streamTxtToHtml("/book.txt", out, "/cache", &error));
    EXPECT_EQ(error, txt_encoding::ReadError::None);
    const auto offset = 2 + text.find(u"第二章") * 2;
    EXPECT_NE(out.text.find("第二章 后续"), std::string::npos);
    EXPECT_NE(out.text.find("id=\"txt-" + std::to_string(offset) + "\""), std::string::npos);
    HalFile mapping;
    ASSERT_TRUE(Storage.openFileForRead("TEST", "/cache/txt-map.bin", mapping));
    uint32_t visible = 0, source = 0;
    ASSERT_TRUE(txt_progress::resolve(mapping, raw.size(), offset, visible));
    ASSERT_TRUE(txt_progress::sourceForVisible(mapping, raw.size(), visible, source));
    EXPECT_EQ(source, offset);
    mapping.close();
    Sink warm;
    ASSERT_TRUE(Txt::streamTxtToHtml("/book.txt", warm, "/cache", &error));
    EXPECT_EQ(warm.text, out.text);
  }
}
TEST_F(Pipeline, InvalidDataAndIoHaveDistinctErrors) {
  put(std::string("\xff\xfe\x00\xd8", 4));
  Sink out;
  txt_encoding::ReadError e = txt_encoding::ReadError::None;
  EXPECT_FALSE(Txt::streamTxtToHtml("/book.txt", out, "/cache", &e));
  EXPECT_EQ(e, txt_encoding::ReadError::InvalidData);
  EXPECT_FALSE(std::filesystem::exists(root / "cache/txt-map.bin"));
  EXPECT_FALSE(Txt::streamTxtToHtml("/absent.txt", out, "/cache", &e));
  EXPECT_EQ(e, txt_encoding::ReadError::Io);
}
}  // namespace
