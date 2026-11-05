#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "CssParser.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxRules = 1500;
constexpr size_t kMaxUniqueStyles = 256;
constexpr size_t kCacheHeaderBytes = sizeof(uint8_t) * 2 + sizeof(uint16_t);
constexpr size_t kStyleEnumPrefixBytes = 5;
constexpr size_t kStyleLengthFieldCount = 14;
constexpr size_t kStyleLengthBytes = sizeof(decltype(CssLength::value)) + sizeof(uint8_t);

class CssParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    directory_ = fs::temp_directory_path() / "crosspoint_css_parser_test" / info->name();
    fs::remove_all(directory_);
    fs::create_directories(directory_);
    Storage.clearFailures();
  }

  void TearDown() override {
    Storage.clearFailures();
    fs::remove_all(directory_);
  }

  std::string cachePath() const { return directory_.string(); }
  fs::path cacheFile() const { return directory_ / "css_rules.cache"; }
  fs::path cacheTempFile() const { return directory_ / "css_rules.cache.tmp"; }
  fs::path cacheBackupFile() const { return directory_ / "css_rules.cache.bak"; }

  std::vector<uint8_t> readCache() const {
    std::ifstream input(cacheFile(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void writeCache(const std::vector<uint8_t>& bytes) const {
    std::ofstream output(cacheFile(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  CssParser::ParseResult loadCss(CssParser& parser, const std::string& css) const {
    const fs::path sourcePath = directory_ / "input.css";
    std::ofstream output(sourcePath, std::ios::binary);
    output.write(css.data(), static_cast<std::streamsize>(css.size()));
    output.close();

    HalFile source;
    EXPECT_TRUE(HalStorage::getInstance().openFileForRead("TST", sourcePath.string(), source));
    return parser.loadFromStream(source, stylesheetPath);
  }

  std::string stylesheetPath;

  fs::path directory_;
};

TEST_F(CssParserTest, ResolvesCaseInsensitiveCascadeAndMergesDuplicates) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "P { text-align: center; }\n"
                    ".Note { font-weight: bold; text-align: right; }\n"
                    "p.note { font-style: italic; text-align: justify; }\n"
                    ".note { margin-top: 2em; }\n"),
            CssParser::ParseResult::Complete);

  EXPECT_EQ(parser.ruleCount(), 3u);
  const CssStyle style = parser.resolveStyle("p", "NOTE");
  EXPECT_EQ(style.textAlign, CssTextAlign::Justify);
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
  ASSERT_TRUE(style.hasMarginTop());
  EXPECT_FLOAT_EQ(style.marginTop.value, 2.0f);
  EXPECT_EQ(style.marginTop.unit, CssUnit::Em);
}

TEST_F(CssParserTest, DeduplicatedStylesReachTheBoundedRuleCap) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxRules + 20; ++i) {
    css += ".class-" + std::to_string(i) + " { font-weight: bold; text-indent: 1.5em; }\n";
  }

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), kMaxRules);
  EXPECT_EQ(parser.resolveStyle("div", "class-1499").fontWeight, CssFontWeight::Bold);
  EXPECT_FALSE(parser.resolveStyle("div", "class-1500").hasFontWeight());
}

TEST_F(CssParserTest, CascadeUpdatesContinueAfterRuleCap) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxRules + 20; ++i) {
    css += ".class-" + std::to_string(i) + " { font-weight: bold; }\n";
  }
  css += ".class-1499 { font-style: italic; }\n";

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), kMaxRules);
  const CssStyle style = parser.resolveStyle("div", "class-1499");
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, UniqueStyleCapStopsWithoutCorruptingAcceptedRules) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxUniqueStyles + 20; ++i) {
    css += ".unique-" + std::to_string(i) + " { text-indent: " + std::to_string(i + 1) + "px; }\n";
  }

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), kMaxUniqueStyles);
  EXPECT_FLOAT_EQ(parser.resolveStyle("p", "unique-255").textIndent.value, 256.0f);
  EXPECT_FALSE(parser.resolveStyle("p", "unique-256").hasTextIndent());
}

TEST_F(CssParserTest, RepeatedOverridesReuseAnUnsharedStyleSlot) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxUniqueStyles + 20; ++i) {
    css += ".same { text-indent: " + std::to_string(i + 1) + "px; }\n";
  }

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Complete);
  EXPECT_EQ(parser.ruleCount(), 1u);
  EXPECT_FLOAT_EQ(parser.resolveStyle("p", "same").textIndent.value, static_cast<float>(kMaxUniqueStyles + 20));
}

TEST_F(CssParserTest, OversizedSelectorGroupMarksParsePartialAndSkipsRule) {
  CssParser parser(cachePath());
  const std::string css = "." + std::string(1100, 'a') +
                          " { font-weight: bold; }\n"
                          ".valid { font-style: italic; }\n";

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), 1u);
  EXPECT_EQ(parser.resolveStyle("span", "valid").fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, OversizedDeclarationMarksParsePartialAndKeepsFollowingDeclarations) {
  CssParser parser(cachePath());
  const std::string css = ".long { font-weight: " + std::string(1100, 'x') + "; font-style: italic; }\n";

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), 1u);
  const CssStyle style = parser.resolveStyle("span", "long");
  EXPECT_FALSE(style.hasFontWeight());
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, IncompleteInputMarksParsePartial) {
  for (const char* css : {".a { font-weight: bold;", "@media screen {", "/* unfinished", ".unfinished"}) {
    CssParser parser(cachePath());
    EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial) << css;
  }
}

TEST_F(CssParserTest, CanonicalCacheRoundTripPreservesStyles) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer,
                    "p { text-align: justify; margin-top: 2em; }\n"
                    ".bold { font-weight: bolder; }\n"
                    ".hidden { display: none; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Complete);

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_EQ(reader.ruleCount(), writer.ruleCount());
  EXPECT_EQ(reader.resolveStyle("span", "bold").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(reader.resolveStyle("div", "hidden").display, CssDisplay::None);
  const CssStyle paragraph = reader.resolveStyle("p", "");
  EXPECT_EQ(paragraph.textAlign, CssTextAlign::Justify);
  EXPECT_FLOAT_EQ(paragraph.marginTop.value, 2.0f);
}

TEST_F(CssParserTest, PartialCacheIsValidatedDuringInspection) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { font-weight: bold; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(false));
  ASSERT_EQ(writer.inspectCache(), CssParser::CacheStatus::Partial);

  const auto size = fs::file_size(cacheFile());
  fs::resize_file(cacheFile(), size - 1);
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Invalid);
}

TEST_F(CssParserTest, CompleteCacheDefersPayloadValidationToHydration) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { font-weight: bold; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  const auto size = fs::file_size(cacheFile());
  fs::resize_file(cacheFile(), size - 1);
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Complete);

  CssParser reader(cachePath());
  EXPECT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Invalid);
  EXPECT_TRUE(reader.empty());
}

TEST_F(CssParserTest, FailedPromotionRestoresTheOnlyBackupCache) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".original { font-weight: bold; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  fs::rename(cacheFile(), cacheBackupFile());
  ASSERT_FALSE(fs::exists(cacheFile()));
  ASSERT_EQ(loadCss(writer, ".replacement { font-style: italic; }\n"), CssParser::ParseResult::Complete);

  Storage.failNextRename(cacheTempFile().string(), cacheFile().string());
  EXPECT_FALSE(writer.saveToCache(true));
  EXPECT_TRUE(fs::exists(cacheFile()));
  EXPECT_FALSE(fs::exists(cacheTempFile()));
  EXPECT_FALSE(fs::exists(cacheBackupFile()));

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_EQ(reader.ruleCount(), 1u);
  EXPECT_EQ(reader.resolveStyle("span", "original").fontWeight, CssFontWeight::Bold);
  EXPECT_FALSE(reader.resolveStyle("span", "replacement").hasFontStyle());
}

TEST_F(CssParserTest, CacheHydrationRejectsInvalidStyleEnumBytes) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { font-weight: bold; margin-top: 2em; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  const std::vector<uint8_t> validCache = readCache();
  ASSERT_GE(validCache.size(), kCacheHeaderBytes + sizeof(uint16_t));
  uint16_t selectorLength = 0;
  memcpy(&selectorLength, validCache.data() + kCacheHeaderBytes, sizeof(selectorLength));
  const size_t styleOffset = kCacheHeaderBytes + sizeof(selectorLength) + selectorLength;

  std::vector<size_t> enumOffsets = {0, 1, 2, 3, 4};
  for (size_t i = 0; i < kStyleLengthFieldCount; ++i) {
    enumOffsets.push_back(kStyleEnumPrefixBytes + i * kStyleLengthBytes + sizeof(decltype(CssLength::value)));
  }
  enumOffsets.push_back(kStyleEnumPrefixBytes + kStyleLengthFieldCount * kStyleLengthBytes);
  enumOffsets.push_back(kStyleEnumPrefixBytes + kStyleLengthFieldCount * kStyleLengthBytes + 1);

  for (const size_t enumOffset : enumOffsets) {
    SCOPED_TRACE(enumOffset);
    std::vector<uint8_t> corruptedCache = validCache;
    ASSERT_LT(styleOffset + enumOffset, corruptedCache.size());
    corruptedCache[styleOffset + enumOffset] = 0xff;
    writeCache(corruptedCache);

    CssParser reader(cachePath());
    EXPECT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Invalid);
    EXPECT_TRUE(reader.empty());
  }
}

TEST_F(CssParserTest, CacheHydrationRejectsNonFiniteStyleLengths) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { margin-top: 2em; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  const std::vector<uint8_t> validCache = readCache();
  ASSERT_GE(validCache.size(), kCacheHeaderBytes + sizeof(uint16_t));
  uint16_t selectorLength = 0;
  memcpy(&selectorLength, validCache.data() + kCacheHeaderBytes, sizeof(selectorLength));
  const size_t firstLengthOffset = kCacheHeaderBytes + sizeof(selectorLength) + selectorLength + kStyleEnumPrefixBytes;

  using LengthValue = decltype(CssLength::value);
  for (const LengthValue invalidValue :
       {std::numeric_limits<LengthValue>::quiet_NaN(), std::numeric_limits<LengthValue>::infinity(),
        -std::numeric_limits<LengthValue>::infinity()}) {
    std::vector<uint8_t> corruptedCache = validCache;
    ASSERT_LE(firstLengthOffset + sizeof(invalidValue), corruptedCache.size());
    memcpy(corruptedCache.data() + firstLengthOffset, &invalidValue, sizeof(invalidValue));
    writeCache(corruptedCache);

    CssParser reader(cachePath());
    EXPECT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Invalid);
    EXPECT_TRUE(reader.empty());
  }
}

}  // namespace

TEST_F(CssParserTest, BookTypographySizeColorAndFamily) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "p.ss2 { font-size: 0.8em; color: #696969; font-family: \"mm\", \"kt\", serif; }\n"
                    ".big { font-size: 150%; color: rgb(0, 0, 128); }\n"
                    ".kw { font-size: x-large; color: Maroon; }\n"
                    ".bad { font-size: -2em; color: transparent; }\n"),
            CssParser::ParseResult::Complete);
  const CssStyle ss2 = parser.resolveStyle("p", "ss2");
  ASSERT_TRUE(ss2.hasFontSize());
  EXPECT_FLOAT_EQ(ss2.fontSize.value, 0.8f);
  EXPECT_EQ(ss2.fontSize.unit, CssUnit::Em);
  ASSERT_TRUE(ss2.hasColor());
  EXPECT_EQ(ss2.colorLuma, 105);  // #696969
  ASSERT_TRUE(ss2.hasFontFamily());
  EXPECT_EQ(parser.familyAt(ss2.fontFamily), "\"mm\", \"kt\", serif");

  const CssStyle big = parser.resolveStyle("div", "big");
  EXPECT_EQ(big.fontSize.unit, CssUnit::Percent);
  EXPECT_FLOAT_EQ(big.fontSize.value, 150.0f);
  EXPECT_EQ(big.colorLuma, 15);  // navy: dark
  const CssStyle kw = parser.resolveStyle("span", "kw");
  EXPECT_FLOAT_EQ(kw.fontSize.value, 1.5f);
  EXPECT_EQ(kw.fontSize.unit, CssUnit::Em);
  EXPECT_EQ(kw.colorLuma, 38);  // maroon
  const CssStyle bad = parser.resolveStyle("span", "bad");
  EXPECT_FALSE(bad.hasFontSize());
  EXPECT_FALSE(bad.hasColor());

  // style="" keeps its family only through the parser that owns the pool.
  EXPECT_FALSE(CssParser::parseInlineStyle("font-family: kk").hasFontFamily());
  const CssStyle inlineStyle = parser.parseInlineStyleWithFamilies("font-family: kk; color: #000");
  ASSERT_TRUE(inlineStyle.hasFontFamily());
  EXPECT_EQ(parser.familyAt(inlineStyle.fontFamily), "kk");
  EXPECT_EQ(parser.parseInlineStyleWithFamilies("font-family: kk").fontFamily, inlineStyle.fontFamily);
}

TEST_F(CssParserTest, PanelBackgroundBorderAndRadius) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "div.num3 { padding: 1.2em; background: rgba(153, 255, 204, 0.5); border: 1px solid #33cccc; "
                    "border-radius: 8px; }\n"
                    ".gray { background-color: #ddd; border: none; }\n"
                    ".thick { border: thick dashed; }\n"
                    ".rule { border-width: 0 0 1px 0; border-style: solid; }\n"
                    ".tint { border-color: red; }\n"
                    ".pic { background: url(bg.png) no-repeat; }\n"),
            CssParser::ParseResult::Complete);
  const CssStyle num3 = parser.resolveStyle("div", "num3");
  ASSERT_TRUE(num3.hasBackground());
  EXPECT_EQ(num3.backgroundLuma, 237);  // luma 219 at half alpha over white
  ASSERT_TRUE(num3.hasBorder());
  EXPECT_FLOAT_EQ(num3.borderWidth.value, 1.0f);
  EXPECT_EQ(num3.borderWidth.unit, CssUnit::Pixels);
  EXPECT_EQ(num3.borderLuma, 158);
  ASSERT_TRUE(num3.hasBorderRadius());
  EXPECT_FLOAT_EQ(num3.borderRadius.value, 8.0f);

  const CssStyle gray = parser.resolveStyle("p", "gray");
  EXPECT_EQ(gray.backgroundLuma, 221);
  EXPECT_LE(gray.borderWidth.value, 0.0f);
  EXPECT_FLOAT_EQ(parser.resolveStyle("p", "thick").borderWidth.value, 5.0f);
  EXPECT_LE(parser.resolveStyle("p", "rule").borderWidth.value, 0.0f);  // one side only: not a panel
  EXPECT_LE(parser.resolveStyle("p", "tint").borderWidth.value, 0.0f);  // no style, no border
  EXPECT_FALSE(parser.resolveStyle("p", "pic").hasBackground());
}

TEST_F(CssParserTest, FontFaceRulesResolveAgainstTheirStylesheet) {
  stylesheetPath = "OEBPS/Styles/fonts.css";
  std::string longSrc;
  for (int i = 0; i < 60; ++i) longSrc += "local(\"Song" + std::to_string(i) + "\"),";
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "@font-face { font-family: \"cc\"; src: url(\"../Fonts/cc.ttf\"); }\n"
                    "@font-face { font-family:\"zw\"; src: local(\"宋体\"),local(\"明体\"), " + longSrc +
                        " url(../Fonts/zw.ttf), url(res:///opt/sony/ebook/FONT/zw.ttf); }\n"
                    "@font-face { font-family: kt; src: local(\"楷体\"), local(\"Kaiti SC\"); }\n"
                    "@font-face { font-family: web; src: url(http://example.com/a.ttf); }\n"
                    "@font-face { font-family: \"cc\"; src: url(../Fonts/other.ttf); }\n"
                    "p { font-family: \"cc\"; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_EQ(parser.fontFaceCount(), 4);
  EXPECT_EQ(parser.fontFaceAt(0).family, "cc");
  EXPECT_EQ(parser.fontFaceAt(0).path, "OEBPS/Fonts/cc.ttf");
  EXPECT_EQ(parser.fontFaceAt(1).family, "zw");
  EXPECT_EQ(parser.fontFaceAt(1).path, "OEBPS/Fonts/zw.ttf");
  EXPECT_EQ(parser.fontFaceAt(1).category, CssParser::FontCategory::Serif);
  EXPECT_EQ(parser.fontFaceAt(2).family, "kt");
  EXPECT_EQ(parser.fontFaceAt(2).path, "");
  EXPECT_EQ(parser.fontFaceAt(2).category, CssParser::FontCategory::Kai);
  // A repeated family is kept for the reader to fall through to.
  EXPECT_EQ(parser.fontFaceAt(3).family, "cc");
  EXPECT_EQ(parser.fontFaceAt(3).path, "OEBPS/Fonts/other.ttf");
  // The @font-face font-family names a face; only the p rule selects one.
  EXPECT_EQ(parser.ruleCount(), 1u);
  EXPECT_EQ(parser.familyAt(parser.resolveStyle("p", "").fontFamily), "\"cc\"");
}

TEST_F(CssParserTest, FontNameCategories) {
  using C = CssParser::FontCategory;
  EXPECT_EQ(CssParser::classifyFontName("宋体"), C::Serif);
  EXPECT_EQ(CssParser::classifyFontName("DK-XIAOBIAOSONG"), C::Serif);
  EXPECT_EQ(CssParser::classifyFontName("仿宋_GB2312"), C::FangSong);
  EXPECT_EQ(CssParser::classifyFontName("STKaiti"), C::Kai);
  EXPECT_EQ(CssParser::classifyFontName("DK-HEITI"), C::Sans);
  EXPECT_EQ(CssParser::classifyFontName("sans-serif"), C::Sans);
  EXPECT_EQ(CssParser::classifyFontName("serif"), C::Serif);
  EXPECT_EQ(CssParser::classifyFontName("Yuanti SC"), C::Round);
  EXPECT_EQ(CssParser::classifyFontName("Caecilia"), C::None);
}

TEST_F(CssParserTest, CacheKeepsFamiliesAndFontFaces) {
  stylesheetPath = "OEBPS/Styles/main.css";
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer,
                    "@font-face { font-family: mm; src: url(../Fonts/mm.ttf); }\n"
                    ".a { font-family: \"kk\", serif; font-size: 1.1em; color: #333; }\n"
                    ".b { font-family: mm; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  const CssStyle a = reader.resolveStyle("p", "a");
  EXPECT_EQ(reader.familyAt(a.fontFamily), "\"kk\", serif");
  EXPECT_FLOAT_EQ(a.fontSize.value, 1.1f);
  EXPECT_EQ(a.colorLuma, 51);
  EXPECT_EQ(reader.familyAt(reader.resolveStyle("p", "b").fontFamily), "mm");
  ASSERT_EQ(reader.fontFaceCount(), 1);
  EXPECT_EQ(reader.fontFaceAt(0).path, "OEBPS/Fonts/mm.ttf");
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Complete);
}

TEST_F(CssParserTest, DescendantSelectorsMatchAncestors) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "p { font-size: 1em; }\n"
                    "div.bt p { font-family: mm; font-size: 0.85em; }\n"
                    "h2.mm > b { color: #800000; }\n"
                    "div.a.b span { color: #666666; }\n"
                    "a:link { color: #366; }\n"),
            CssParser::ParseResult::Complete);
  EXPECT_TRUE(parser.hasDescendantRules());
  const CssParser::ElementRef inBt[] = {{"div", "bt"}, {"body", ""}};
  const CssStyle p = parser.resolveStyle("p", "", inBt, 2);
  EXPECT_EQ(parser.familyAt(p.fontFamily), "mm");
  EXPECT_FLOAT_EQ(p.fontSize.value, 0.85f);
  const CssParser::ElementRef plain[] = {{"div", "other"}, {"body", ""}};
  EXPECT_FALSE(parser.resolveStyle("p", "", plain, 2).hasFontFamily());
  EXPECT_FLOAT_EQ(parser.resolveStyle("p", "", plain, 2).fontSize.value, 1.0f);
  // '>' matched as a descendant; tags are case-insensitive.
  const CssParser::ElementRef deep[] = {{"span", ""}, {"H2", "mm x"}};
  EXPECT_EQ(parser.resolveStyle("b", "", deep, 2).colorLuma, 38);
  // Every class of a compound must be present.
  const CssParser::ElementRef onlyA[] = {{"div", "a"}};
  const CssParser::ElementRef both[] = {{"p", ""}, {"div", "b a"}};
  EXPECT_FALSE(parser.resolveStyle("span", "", onlyA, 1).hasColor());
  EXPECT_EQ(parser.resolveStyle("span", "", both, 2).colorLuma, 102);
  // Without ancestors, the plain lookup ignores descendant rules.
  EXPECT_FALSE(parser.resolveStyle("p", "").hasFontFamily());

  ASSERT_TRUE(parser.saveToCache(true));
  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_EQ(reader.familyAt(reader.resolveStyle("p", "", inBt, 2).fontFamily), "mm");
}
