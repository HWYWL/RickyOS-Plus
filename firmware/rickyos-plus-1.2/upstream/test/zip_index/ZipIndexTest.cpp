#include <HalStorage.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ZipFile.h"

// tinflate.c references the checksum helpers the firmware links from elsewhere; stored
// entries never reach them.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t prev) { return prev; }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t prev) { return prev; }

namespace {
// Minimal STORED archive writer: local headers, central directory, EOCD.
void writeZip(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<uint8_t> out, cd;
  const auto le16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF), v.push_back((x >> 8) & 0xFF); };
  const auto le32 = [&](std::vector<uint8_t>& v, uint32_t x) { le16(v, x & 0xFFFF), le16(v, x >> 16); };
  for (const auto& [name, data] : files) {
    const uint32_t offset = static_cast<uint32_t>(out.size());
    le32(out, 0x04034b50), le16(out, 20), le16(out, 0), le16(out, 0), le32(out, 0), le32(out, 0);
    le32(out, data.size()), le32(out, data.size()), le16(out, name.size()), le16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
    le32(cd, 0x02014b50), le16(cd, 20), le16(cd, 20), le16(cd, 0), le16(cd, 0), le32(cd, 0), le32(cd, 0);
    le32(cd, data.size()), le32(cd, data.size()), le16(cd, name.size()), le16(cd, 0), le16(cd, 0);
    le16(cd, 0), le16(cd, 0), le32(cd, 0), le32(cd, offset);
    cd.insert(cd.end(), name.begin(), name.end());
  }
  const uint32_t cdOffset = static_cast<uint32_t>(out.size());
  out.insert(out.end(), cd.begin(), cd.end());
  le32(out, 0x06054b50), le16(out, 0), le16(out, 0), le16(out, files.size()), le16(out, files.size());
  le32(out, cd.size()), le32(out, cdOffset), le16(out, 0);
  std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.data()), out.size());
}

class ZipIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> serial{0};
    root = std::filesystem::temp_directory_path() / ("crossmux-zip-index-" + std::to_string(serial++));
    std::filesystem::remove_all(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    ASSERT_EQ(setenv("CROSSPOINT_SIM_SD", root.c_str(), 1), 0);
    ASSERT_TRUE(Storage.begin());
  }
  void TearDown() override {
    unsetenv("CROSSPOINT_SIM_SD");
    std::filesystem::remove_all(root);
  }
  std::string read(const std::string& archive, const std::string& name) {
    size_t size = 0;
    uint8_t* bytes = ZipFile(archive).readFileToMemory(name.c_str(), &size);
    if (!bytes) return "<missing>";
    std::string text(reinterpret_cast<char*>(bytes), size);
    free(bytes);
    return text;
  }
  std::filesystem::path root;
};

std::vector<std::pair<std::string, std::string>> book(const int entries, const char* tag) {
  std::vector<std::pair<std::string, std::string>> files;
  for (int i = 0; i < entries; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "OEBPS/Styles/style%04d.css", i);
    files.emplace_back(name, std::string(tag) + std::to_string(i));
  }
  return files;
}

TEST_F(ZipIndexTest, EveryEntryResolvesInAnyOrder) {
  writeZip(root / "big.epub", book(3000, "a"));
  const std::string archive = "/big.epub";
  for (int i = 2999; i >= 0; i -= 7) {
    char name[64];
    snprintf(name, sizeof(name), "OEBPS/Styles/style%04d.css", i);
    EXPECT_EQ(read(archive, name), "a" + std::to_string(i));
    size_t size = 0;
    EXPECT_TRUE(ZipFile(archive).getInflatedFileSize(name, &size));
    EXPECT_EQ(size, ("a" + std::to_string(i)).size());
  }
  EXPECT_EQ(read(archive, "OEBPS/Styles/style3000.css"), "<missing>");
  EXPECT_EQ(read(archive, "OEBPS/Styles/style0001.cs"), "<missing>");
  EXPECT_EQ(read(archive, ""), "<missing>");
}

TEST_F(ZipIndexTest, ReplacedArchiveIsReindexed) {
  writeZip(root / "book.epub", book(40, "old"));
  EXPECT_EQ(read("/book.epub", "OEBPS/Styles/style0007.css"), "old7");
  auto files = book(41, "new");  // different entry count and size
  writeZip(root / "book.epub", files);
  EXPECT_EQ(read("/book.epub", "OEBPS/Styles/style0007.css"), "new7");
  EXPECT_EQ(read("/book.epub", "OEBPS/Styles/style0040.css"), "new40");
}

TEST_F(ZipIndexTest, AlternatingArchivesStayCorrect) {
  writeZip(root / "one.epub", book(120, "one"));
  writeZip(root / "two.epub", book(90, "two"));
  writeZip(root / "three.epub", book(60, "three"));
  for (int round = 0; round < 3; ++round) {
    EXPECT_EQ(read("/one.epub", "OEBPS/Styles/style0100.css"), "one100");
    EXPECT_EQ(read("/two.epub", "OEBPS/Styles/style0050.css"), "two50");
    EXPECT_EQ(read("/three.epub", "OEBPS/Styles/style0059.css"), "three59");
    EXPECT_EQ(read("/two.epub", "OEBPS/Styles/style0100.css"), "<missing>");
  }
}

TEST_F(ZipIndexTest, OpenArchiveResolvesManyItems) {
  writeZip(root / "open.epub", book(500, "x"));
  const std::string archive = "/open.epub";
  ZipFile zip(archive);
  ASSERT_TRUE(zip.open());
  for (int i = 0; i < 500; i += 3) {
    char name[64];
    snprintf(name, sizeof(name), "OEBPS/Styles/style%04d.css", i);
    size_t size = 0;
    uint8_t* bytes = zip.readFileToMemory(name, &size);
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(bytes), size), "x" + std::to_string(i));
    free(bytes);
  }
  zip.close();
}
}  // namespace
