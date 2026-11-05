#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "MarkdownFormatter.h"
#include "TxtToHtml.h"

namespace {
class Output : public Print {
 public:
  std::string text;
  size_t write(uint8_t value) override {
    text.push_back(static_cast<char>(value));
    return 1;
  }
  size_t write(const uint8_t* bytes, size_t count) override {
    text.append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
};
struct Reader {
  std::string_view text;
  size_t offset = 0;
  size_t chunk = 1;
  static int read(void* opaque, uint8_t* bytes, size_t capacity) {
    auto& self = *static_cast<Reader*>(opaque);
    const size_t count = std::min({self.chunk, capacity, self.text.size() - self.offset});
    memcpy(bytes, self.text.data() + self.offset, count);
    self.offset += count;
    return static_cast<int>(count);
  }
};
std::string convert(std::string_view text, size_t chunk = 1) {
  Reader reader{text, 0, chunk};
  Output out;
  EXPECT_TRUE(TxtToHtml::stream("中文.MD", &reader, Reader::read, out));
  return out.text;
}

TEST(Markdown, CommonReadingBlocksAndInlineStyles) {
  const std::string source =
      "# 阅读与生活\n\n## 安静的一页\n中文 **粗体** 和 *斜体* 与 `a<b`。\n"
      "- 喝杯茶\n- 再读一页\n\n1. 清晨\n2. 夜晚\n\n> 慢一点。\n---\n";
  for (size_t chunk : {1U, 7U, 4096U, 8192U}) {
    const auto html = convert(source, chunk);
    EXPECT_NE(html.find("<!-- MD_CACHE_VERSION: 6 -->"), std::string::npos);
    EXPECT_NE(html.find("<h1>阅读与生活</h1>"), std::string::npos);
    EXPECT_NE(html.find("<h2>安静的一页</h2>"), std::string::npos);
    EXPECT_NE(html.find("<strong>粗体</strong>"), std::string::npos);
    EXPECT_NE(html.find("<em>斜体</em>"), std::string::npos);
    EXPECT_NE(html.find("<code>a&lt;b</code>"), std::string::npos);
    EXPECT_NE(html.find("<ul><li>喝杯茶</li><li>再读一页</li></ul>"), std::string::npos);
    EXPECT_NE(html.find("<ol><li>清晨</li><li>夜晚</li></ol>"), std::string::npos);
    EXPECT_NE(html.find("<blockquote>慢一点。</blockquote>"), std::string::npos);
    EXPECT_NE(html.find("<hr />"), std::string::npos);
  }
}

TEST(Markdown, FencesEscapesLinksAndRawHtmlAreSafe) {
  const auto html = convert(
      "~~~cpp\n  **literal** <script>&\n~~~\n"
      "\\*literal\\* [官网](https://example.test/) <img src=\"/secret\"/>\n"
      "```\n未关闭的代码\n");
  EXPECT_NE(html.find("&#160;&#160;**literal**&#160;&lt;script&gt;&amp;<br />"), std::string::npos);
  EXPECT_NE(html.find("*literal* 官网 &lt;img"), std::string::npos);
  EXPECT_EQ(html.find("https://example.test/"), std::string::npos);
  EXPECT_EQ(html.find("<script>"), std::string::npos);
  EXPECT_EQ(html.find("<img "), std::string::npos);
  EXPECT_NE(html.find("未关闭的代码<br /></code></div>"), std::string::npos);
}

TEST(Markdown, LongLinesNeverTruncateAndTxtConversionIsUnchanged) {
  const std::string longLine = "**" + std::string(12000, 'x') + "中文**";
  const auto html = convert(longLine, 7);
  EXPECT_NE(html.find(longLine), std::string::npos);  // Bounded fallback, not partial formatting.
  EXPECT_NE(html.find("</p>\n</body>"), std::string::npos);
  Output plain;
  ASSERT_TRUE(TxtToHtml::stream("plain.txt", "# 标题\n**普通文本**", plain));
  EXPECT_NE(plain.text.find("# 标题<br />**普通文本**"), std::string::npos);
  EXPECT_NE(plain.text.find("<!-- TXT_CACHE_VERSION: 3 -->"), std::string::npos);
}

TEST(Markdown, UnmatchedSyntaxAndLiteralHashtagsRemainReadable) {
  const auto html = convert("#话题\n******* not heading\n未闭合 **粗体\n[链接](unfinished\n");
  EXPECT_NE(html.find("#话题"), std::string::npos);
  EXPECT_NE(html.find("未闭合 **粗体"), std::string::npos);
  EXPECT_NE(html.find("[链接](unfinished"), std::string::npos);
  EXPECT_NE(convert("file_name_here").find("file_name_here"), std::string::npos);
  EXPECT_NE(convert("### 标题 ###\r\n").find("<h3>标题</h3>"), std::string::npos);
  EXPECT_NE(convert("![文字替代](https://example.test/a.jpg)").find("<p>文字替代</p>"), std::string::npos);
}

TEST(Markdown, SourceMapIsMonotonicAndOmitsSyntaxRatherThanContent) {
  struct Point {
    uint32_t source, visible;
    uint8_t width, step;
  };
  struct State {
    std::vector<Point> points;
    unsigned chapters = 0;
  } state;
  const std::string source = "# 中文\r\n\n**粗体** & `码`\n- 列表\n";
  Reader reader{source};
  Output out;
  TxtToHtml::Options options;
  options.context = &state;
  options.map = [](void* opaque, uint32_t source, uint32_t visible, uint8_t width, uint8_t step) {
    auto& state = *static_cast<State*>(opaque);
    if (!state.points.empty()) {
      EXPECT_GT(source, state.points.back().source);
      EXPECT_GE(visible, state.points.back().visible);
    }
    state.points.push_back({source, visible, width, step});
    return true;
  };
  options.chapterOffset = [](void* opaque) { return static_cast<State*>(opaque)->chapters ? UINT32_MAX : 0U; };
  options.nextChapter = [](void* opaque) {
    ++static_cast<State*>(opaque)->chapters;
    return true;
  };
  ASSERT_TRUE(TxtToHtml::stream("map.md", &reader, Reader::read, out, options));
  EXPECT_EQ(state.chapters, 1U);
  EXPECT_NE(out.text.find("<div id=\"txt-0\"></div><h1>中文</h1>"), std::string::npos);
  EXPECT_EQ(state.points.back().source, source.size());
  EXPECT_EQ(state.points.back().visible, 1U + 2U + 2U + 3U + 1U + 2U);
  for (const auto& point : state.points) {
    if (point.source == source.find("粗")) EXPECT_EQ(point.step, 1);
    if (point.source == source.find("**")) EXPECT_EQ(point.step, 0);
  }
}

TEST(Markdown, GbkSourceWidthsAndReadWriteFailures) {
  const std::string source = "# \xD6\xD0\xCE\xC4\n";
  Reader reader{source};
  Output out;
  TxtToHtml::Options options;
  options.encoding = txt_encoding::Encoding::Gbk;
  ASSERT_TRUE(TxtToHtml::stream("gbk.md", &reader, Reader::read, out, options));
  EXPECT_NE(out.text.find("<h1>中文</h1>"), std::string::npos);
  Output invalid;
  EXPECT_FALSE(TxtToHtml::stream("bad.md", "# \xE4\xB8", invalid));
  EXPECT_FALSE(TxtToHtml::stream("bad.md", nullptr, [](void*, uint8_t*, size_t) { return -1; }, invalid));
  class Full : public Print {
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
  } full;
  EXPECT_FALSE(TxtToHtml::stream("bad.md", "# 内容", full));
  options.map = [](void*, uint32_t, uint32_t, uint8_t, uint8_t) { return false; };
  Reader failed{"**内容**"};
  EXPECT_FALSE(TxtToHtml::stream("bad.md", &failed, Reader::read, invalid, options));
}

TEST(Markdown, TablesWithChineseAlignmentAndInlineStyles) {
  const std::string source =
      "| 项目 | 时间 | 状态 |\r\n| :--- | :---: | ---: |\r\n"
      "| **清晨** | `08:00` | *在读* |\r\n| 夜晚 | 20:00 | 未读 |\r\n\r\n后续正文\n";
  for (size_t chunk : {1U, 7U, 1024U, 8192U}) {
    const auto html = convert(source, chunk);
    EXPECT_NE(html.find("<table><tr><th style=\"text-align:left\">项目</th>"), std::string::npos);
    EXPECT_NE(html.find("<th style=\"text-align:center\">时间</th>"), std::string::npos);
    EXPECT_NE(html.find("<th style=\"text-align:right\">状态</th>"), std::string::npos);
    EXPECT_NE(html.find("<td style=\"text-align:left\"><strong>清晨</strong></td>"), std::string::npos);
    EXPECT_NE(html.find("<td style=\"text-align:center\"><code>08:00</code></td>"), std::string::npos);
    EXPECT_NE(html.find("<td style=\"text-align:right\"><em>在读</em></td>"), std::string::npos);
    EXPECT_NE(html.find("</table><br /><p>后续正文</p>"), std::string::npos);
    EXPECT_EQ(html.find("---"), std::string::npos);
  }
}

TEST(Markdown, TablesAllowOptionalOuterPipesEmptyCellsAndNoFinalNewline) {
  const auto html = convert("项目 | 状态\n--- | ---\n一 |\n| | 在读 |\n二 | 完成");
  EXPECT_NE(html.find("<table>"), std::string::npos);
  EXPECT_NE(html.find("<td style=\"text-align:left\">一</td><td style=\"text-align:left\"></td>"), std::string::npos);
  EXPECT_NE(html.find("<td style=\"text-align:left\"></td><td style=\"text-align:left\">在读</td>"), std::string::npos);
  EXPECT_NE(html.find("完成</td></tr></table>\n</body>"), std::string::npos);
  EXPECT_NE(convert("| 名称 |\n| --- |\n| 中文 |").find("中文</td></tr></table>"), std::string::npos);
}

TEST(Markdown, EscapedAndCodePipesDoNotSplitCellsAndHtmlIsEscaped) {
  const auto html = convert("| A | B |\n|---|---|\n| a\\|b | `x|y` |\n| <script> | & |\n");
  EXPECT_NE(html.find("<td style=\"text-align:left\">a|b</td>"), std::string::npos);
  EXPECT_NE(html.find("<td style=\"text-align:left\"><code>x|y</code></td>"), std::string::npos);
  EXPECT_NE(html.find("&lt;script&gt;</td>"), std::string::npos);
  EXPECT_NE(html.find("&amp;</td>"), std::string::npos);
  EXPECT_EQ(html.find("<script>"), std::string::npos);
}

TEST(Markdown, PipeTextInvalidDelimitersAndFencesNeverBecomeTables) {
  for (const auto source : {"a | b\nnot a table\n", "a | b\n---|invalid\n", "a | b\n---|---|---\n",
                            "````\n| a | b |\n|---|---|\n````\n", "a | b"}) {
    const auto html = convert(source);
    EXPECT_EQ(html.find("<table>"), std::string::npos) << source;
    EXPECT_TRUE(html.find("a | b") != std::string::npos || html.find("a&#160;|&#160;b") != std::string::npos) << source;
  }
}

TEST(Markdown, TableBoundsFallBackWithoutTruncatingExtraOrLongCells) {
  const auto extra = convert("| A | B |\n|---|---|\n| one | two | KEEP EXTRA |\n");
  EXPECT_NE(extra.find("KEEP EXTRA"), std::string::npos);
  const std::string longRow = "| " + std::string(14000, 'x') + " | KEEP END |";
  const auto large = convert("| A | B |\n|---|---|\n" + longRow + "\n");
  EXPECT_NE(large.find(longRow), std::string::npos);
  EXPECT_NE(large.find("</table><p>"), std::string::npos);
  std::string header = "|", separator = "|", row = "|";
  for (unsigned i = 0; i < 17; ++i) {
    header += "列|";
    separator += "---|";
    row += "内容|";
  }
  const auto wide = convert(header + "\n" + separator + "\n" + row);
  EXPECT_EQ(wide.find("<table>"), std::string::npos);
  EXPECT_NE(wide.find(row), std::string::npos);
  const std::string bigHeader = "|" + std::string(3100, 'h') + "|B|";
  EXPECT_NE(convert(bigHeader + "\n|---|---|\n").find(bigHeader), std::string::npos);
}

TEST(Markdown, TableLookaheadMapsEverySourceCharacterInOrder) {
  for (const auto& source :
       std::initializer_list<std::string>{"| 表头 | 内容 |\r\n|---|---|\r\n| 一 | **二** |\n", "a | b\nnot table\n",
                                          "a | b", "| A | B |\n|---|---|\n|" + std::string(5000, 'x') + "|保留|\n"}) {
    struct State {
      uint32_t next = 0, visible = 1;
    } state;
    TxtToHtml::Options options;
    options.context = &state;
    options.map = [](void* opaque, uint32_t source, uint32_t visible, uint8_t width, uint8_t step) {
      auto& state = *static_cast<State*>(opaque);
      EXPECT_EQ(source, state.next);
      EXPECT_EQ(visible, state.visible);
      state.next += width;
      state.visible += step;
      return true;
    };
    Reader reader{source};
    Output out;
    ASSERT_TRUE(TxtToHtml::stream("map.md", &reader, Reader::read, out, options));
    EXPECT_EQ(state.next, source.size());
  }
}

TEST(Markdown, GbkTablesRetainSourceWidthsAndUtf8BomIsIgnored) {
  const std::string source = "| \xD6\xD0\xCE\xC4 | B |\n|---|---|\n| \xD6\xD0 | **ok** |";
  for (size_t chunk : {1U, 7U, 1024U}) {
    struct State {
      uint32_t next = 0;
      unsigned doubleBytes = 0;
    } state;
    TxtToHtml::Options options;
    options.encoding = txt_encoding::Encoding::Gbk;
    options.context = &state;
    options.map = [](void* opaque, uint32_t source, uint32_t, uint8_t width, uint8_t) {
      auto& state = *static_cast<State*>(opaque);
      EXPECT_EQ(source, state.next);
      state.next += width;
      if (width == 2) ++state.doubleBytes;
      return true;
    };
    Reader reader{source, 0, chunk};
    Output out;
    ASSERT_TRUE(TxtToHtml::stream("gbk.md", &reader, Reader::read, out, options));
    EXPECT_NE(out.text.find("中文</th>"), std::string::npos);
    EXPECT_NE(out.text.find("中</td>"), std::string::npos);
    EXPECT_NE(out.text.find("<strong>ok</strong>"), std::string::npos);
    EXPECT_EQ(state.next, source.size());
    EXPECT_EQ(state.doubleBytes, 3U);
  }
  const auto html = convert("\xEF\xBB\xBF| 中文 | B |\n|---|---|\n| 一 | 二 |\n");
  EXPECT_NE(html.find("<table>"), std::string::npos);
  EXPECT_EQ(html.find("\xEF\xBB\xBF"), std::string::npos);
}
}  // namespace

TEST(Markdown, ChapterAnchorsFollowPendingTextAndCloseTables) {
  for (const std::string prefix : {"before | not a table\n", "| A | B |\n| --- | --- |\n| x | y |\n"}) {
    const std::string source = prefix + "##\t**第二节** ##\n正文";
    Reader reader{source};
    struct State {
      uint32_t offset;
    } state{static_cast<uint32_t>(prefix.size())};
    TxtToHtml::Options options;
    options.context = &state;
    options.chapterOffset = [](void* opaque) { return static_cast<State*>(opaque)->offset; };
    options.nextChapter = [](void* opaque) {
      static_cast<State*>(opaque)->offset = UINT32_MAX;
      return true;
    };
    Output out;
    ASSERT_TRUE(TxtToHtml::stream("toc.md", &reader, Reader::read, out, options));
    const auto anchor = out.text.find("<div id=\"txt-");
    const auto heading = out.text.find("<h2><strong>第二节</strong></h2>");
    ASSERT_NE(anchor, std::string::npos);
    EXPECT_LT(anchor, heading);
    EXPECT_LT(out.text.find(prefix.front() == '|' ? "</table>" : "</p>"), anchor);
  }
}

TEST(Markdown, TocLabelsReuseInlineParserAndCanWriteInPlace) {
  char title[192] = "## **中文** / `code` / [链接](https://example.test) / foo_bar / \\*";
  MarkdownFormatter::plainText(std::string_view(title).substr(3), title);
  EXPECT_STREQ(title, "中文 / code / 链接 / foo_bar / *");
  MarkdownFormatter::plainText(title, title);
  EXPECT_STREQ(title, "中文 / code / 链接 / foo_bar / *");
  MarkdownFormatter::plainText(std::string(192, 'x'), title);
  EXPECT_STREQ(title, "");
  const std::string incomplete = std::string(190, 'x') + '\xE4';
  MarkdownFormatter::plainText(incomplete, title);
  EXPECT_EQ(std::string(title), incomplete);
}

TEST(Markdown, Utf16LeBeTablesInlineAndPendingNewlineCoordinates) {
  const std::u16string text =
      u"# 标题\r\n\n| 中文 | 猫\U0001f431 |\r\n| -- | -- |\n| **字** | [链接](x) |\n\n- 项目\n尾";
  const std::string expected =
      convert("# 标题\r\n\n| 中文 | 猫🐱 |\r\n| -- | -- |\n| **字** | [链接](x) |\n\n- 项目\n尾");
  for (bool big : {false, true})
    for (size_t chunk : {1U, 3U, 1023U}) {
      std::string raw;
      raw += big ? std::string("\xfe\xff", 2) : std::string("\xff\xfe", 2);
      for (char16_t c : text) {
        raw.push_back(big ? c >> 8 : c & 255);
        raw.push_back(big ? c & 255 : c >> 8);
      }
      Reader reader{raw, 0, chunk};
      Output out;
      TxtToHtml::Options opts;
      opts.encoding = big ? txt_encoding::Encoding::Utf16Be : txt_encoding::Encoding::Utf16Le;
      uint32_t end = 0;
      opts.context = &end;
      opts.map = [](void* p, uint32_t source, uint32_t, uint8_t width, uint8_t) {
        EXPECT_EQ(source % 2, 0U);
        EXPECT_TRUE(width == 0 || width == 2 || width == 4);
        if (!width) *static_cast<uint32_t*>(p) = source;
        return true;
      };
      ASSERT_TRUE(TxtToHtml::stream("中文.MD", &reader, Reader::read, out, opts));
      EXPECT_EQ(out.text, expected);
      EXPECT_EQ(end, raw.size());
    }
}
