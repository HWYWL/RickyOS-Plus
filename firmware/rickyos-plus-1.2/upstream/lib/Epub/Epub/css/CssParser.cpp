#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  bool push_back(char c) {
    if (len >= CAPACITY) return false;
    data[len++] = c;
    return true;
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }
  operator std::string_view() const noexcept { return view(); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Flat rule-store caps. The index is 12KB at MAX_RULES, selector text is
// bounded to 32KB, and deduplicated style bodies are bounded to about 26KB.
constexpr size_t MAX_RULES = 1500;
constexpr size_t SELECTOR_POOL_CAP = 32 * 1024;
constexpr size_t MAX_UNIQUE_STYLES = 256;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

// Check if character is CSS whitespace
constexpr bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr std::string_view trimCssWhitespace(std::string_view s) {
  while (!s.empty() && isCssWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isCssWhitespace(s.back())) s.remove_suffix(1);
  return s;
}

constexpr char asciiToLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive equality on ASCII. lowercaseKeyword MUST already be
// lowercase; CSS keywords are ASCII by spec so byte-wise tolower is safe.
constexpr bool iequalsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::equal(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                    [](char a, char b) { return asciiToLower(a) == b; });
}

// Walk s and invoke fn(token) for each non-empty run between delimiters.
// Tokens are boundary-trimmed and yielded as string_views into s; no
// allocation. Runs of consecutive delimiters coalesce — no empty tokens are
// emitted. `isDelimiter` is invoked once per character.
template <typename Pred, typename F>
void forEachDelimitedToken(std::string_view s, Pred isDelimiter, F&& fn) {
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || isDelimiter(s[i])) {
      const std::string_view trimmed = trimCssWhitespace(s.substr(start, i - start));
      if (!trimmed.empty()) {
        fn(trimmed);
      }
      start = i + 1;
    }
  }
}

// Parse the entirety of s as an integer into `out`. Accepts an optional leading
// '+' (which std::from_chars rejects by spec) so callers can pass CSS-style
// signed numbers without manual trimming. Returns false on empty input, a
// non-numeric suffix, or any from_chars error.
template <typename T>
bool tryParseNumber(std::string_view s, T& out) {
  const char* begin = s.data();
  const char* end = s.data() + s.size();
  if (begin < end && *begin == '+') ++begin;
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

// Float companion to tryParseNumber. std::from_chars(float) instantiates libstdc++'s
// Eisel-Lemire path, which links a 10 KB power-of-five table for a single call site;
// strtod costs a fraction of that and parses the same values exactly. To keep the two
// interchangeable, this reproduces from_chars' contract: no leading whitespace, an
// optional '+'/'-', digits with at most one '.', at least one digit overall, and the
// entire view consumed.
bool tryParseFloat(std::string_view s, float& out) {
  if (s.empty()) return false;

  size_t i = 0;
  if (s[i] == '+' || s[i] == '-') ++i;

  bool anyDigit = false;
  for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) anyDigit = true;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) anyDigit = true;
  }
  if (!anyDigit || i != s.size()) return false;

  // digits are ASCII, so the view can be handed to strtod once copied into a
  // NUL-terminated buffer. A CSS length is short; a bounded stack copy avoids the
  // allocation a std::string would need on this parser's section-build path.
  constexpr size_t MAX_NUMBER_LENGTH = 32;
  if (s.size() >= MAX_NUMBER_LENGTH) return false;
  char buffer[MAX_NUMBER_LENGTH];
  std::memcpy(buffer, s.data(), s.size());
  buffer[s.size()] = '\0';

  char* parseEnd = nullptr;
  const double value = std::strtod(buffer, &parseEnd);
  if (parseEnd != buffer + s.size()) return false;
  out = static_cast<float>(value);
  return true;
}

// Collect up to 4 whitespace-separated tokens for a CSS edge-value shorthand
// (margin, padding, and the border-* family). Returns the number of tokens
// written; extras are silently dropped. Callers apply the 1/2/3/4-value
// fallback rule using the returned count.
size_t collectEdgeValueTokens(std::string_view s, std::string_view (&out)[4]) {
  size_t count = 0;
  forEachDelimitedToken(s, isCssWhitespace, [&](std::string_view tok) {
    if (count < 4) out[count++] = tok;
  });
  return count;
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (!iequalsAscii(value.substr(suffixPos), IMPORTANT)) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

constexpr std::array STYLE_LENGTH_FIELDS = {
    &CssStyle::textIndent,   &CssStyle::marginTop,   &CssStyle::marginBottom,  &CssStyle::marginLeft,
    &CssStyle::marginRight,  &CssStyle::paddingTop,  &CssStyle::paddingBottom, &CssStyle::paddingLeft,
    &CssStyle::paddingRight, &CssStyle::imageHeight, &CssStyle::imageWidth,  &CssStyle::fontSize,
    &CssStyle::borderWidth,  &CssStyle::borderRadius,
};
constexpr size_t STYLE_LENGTH_FIELD_COUNT = STYLE_LENGTH_FIELDS.size();
// ... + display, verticalAlign, listStyleType + colorLuma, fontFamily (u16), backgroundLuma,
// borderLuma + defined bits.
constexpr size_t STYLE_WIRE_BYTES =
    5 + STYLE_LENGTH_FIELD_COUNT * (sizeof(decltype(CssLength::value)) + 1) + 3 + 5 + sizeof(uint32_t);
constexpr uint32_t CSS_DEFINED_BITS_MASK = (1u << 25) - 1;

void encodeStyleWire(const CssStyle& style, uint8_t (&out)[STYLE_WIRE_BYTES]) {
  size_t offset = 0;
  out[offset++] = static_cast<uint8_t>(style.textAlign);
  out[offset++] = static_cast<uint8_t>(style.fontStyle);
  out[offset++] = static_cast<uint8_t>(style.fontWeight);
  out[offset++] = static_cast<uint8_t>(style.textDecoration);
  out[offset++] = static_cast<uint8_t>(style.direction);

  const auto putLength = [&out, &offset](const CssLength& length) {
    memcpy(out + offset, &length.value, sizeof(length.value));
    offset += sizeof(length.value);
    out[offset++] = static_cast<uint8_t>(length.unit);
  };
  for (const auto field : STYLE_LENGTH_FIELDS) {
    putLength(style.*field);
  }
  out[offset++] = static_cast<uint8_t>(style.display);
  out[offset++] = static_cast<uint8_t>(style.verticalAlign);
  out[offset++] = static_cast<uint8_t>(style.listStyleType);
  out[offset++] = style.colorLuma;
  memcpy(out + offset, &style.fontFamily, sizeof(style.fontFamily));
  offset += sizeof(style.fontFamily);
  out[offset++] = style.backgroundLuma;
  out[offset++] = style.borderLuma;

  uint32_t definedBits = 0;
  if (style.defined.textAlign) definedBits |= 1 << 0;
  if (style.defined.fontStyle) definedBits |= 1 << 1;
  if (style.defined.fontWeight) definedBits |= 1 << 2;
  if (style.defined.textDecoration) definedBits |= 1 << 3;
  if (style.defined.textIndent) definedBits |= 1 << 4;
  if (style.defined.marginTop) definedBits |= 1 << 5;
  if (style.defined.marginBottom) definedBits |= 1 << 6;
  if (style.defined.marginLeft) definedBits |= 1 << 7;
  if (style.defined.marginRight) definedBits |= 1 << 8;
  if (style.defined.paddingTop) definedBits |= 1 << 9;
  if (style.defined.paddingBottom) definedBits |= 1 << 10;
  if (style.defined.paddingLeft) definedBits |= 1 << 11;
  if (style.defined.paddingRight) definedBits |= 1 << 12;
  if (style.defined.imageHeight) definedBits |= 1 << 13;
  if (style.defined.imageWidth) definedBits |= 1 << 14;
  if (style.defined.display) definedBits |= 1 << 15;
  if (style.defined.direction) definedBits |= 1 << 16;
  if (style.defined.verticalAlign) definedBits |= 1 << 17;
  if (style.defined.listStyleType) definedBits |= 1 << 18;
  if (style.defined.fontSize) definedBits |= 1 << 19;
  if (style.defined.color) definedBits |= 1 << 20;
  if (style.defined.fontFamily) definedBits |= 1 << 21;
  if (style.defined.background) definedBits |= 1 << 22;
  if (style.defined.border) definedBits |= 1 << 23;
  if (style.defined.borderRadius) definedBits |= 1 << 24;
  memcpy(out + offset, &definedBits, sizeof(definedBits));
}

bool decodeStyleWire(const uint8_t (&in)[STYLE_WIRE_BYTES], CssStyle& style) {
  size_t offset = 0;
  const uint8_t textAlign = in[offset++];
  const uint8_t fontStyle = in[offset++];
  const uint8_t fontWeight = in[offset++];
  const uint8_t textDecoration = in[offset++];
  const uint8_t direction = in[offset++];
  if (textAlign > static_cast<uint8_t>(CssTextAlign::None) || fontStyle > static_cast<uint8_t>(CssFontStyle::Italic) ||
      fontWeight > static_cast<uint8_t>(CssFontWeight::Bold) || (textDecoration & ~CSS_TEXT_DECORATION_MASK) != 0 ||
      direction > static_cast<uint8_t>(CssTextDirection::Rtl)) {
    return false;
  }
  style.textAlign = static_cast<CssTextAlign>(textAlign);
  style.fontStyle = static_cast<CssFontStyle>(fontStyle);
  style.fontWeight = static_cast<CssFontWeight>(fontWeight);
  style.textDecoration = static_cast<CssTextDecoration>(textDecoration);
  style.direction = static_cast<CssTextDirection>(direction);

  const auto getLength = [&in, &offset](CssLength& length) {
    decltype(CssLength::value) value = 0;
    memcpy(&value, in + offset, sizeof(value));
    offset += sizeof(length.value);
    const uint8_t unit = in[offset++];
    if (!std::isfinite(value) || unit > static_cast<uint8_t>(CssUnit::Percent)) return false;
    length.value = value;
    length.unit = static_cast<CssUnit>(unit);
    return true;
  };
  for (const auto field : STYLE_LENGTH_FIELDS) {
    if (!getLength(style.*field)) return false;
  }

  const uint8_t display = in[offset++];
  const uint8_t verticalAlign = in[offset++];
  const uint8_t listStyleType = in[offset++];
  if (display > static_cast<uint8_t>(CssDisplay::None) || verticalAlign > static_cast<uint8_t>(CssVerticalAlign::Sub) ||
      listStyleType > static_cast<uint8_t>(CssListStyleType::None)) {
    return false;
  }
  style.display = static_cast<CssDisplay>(display);
  style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlign);
  style.listStyleType = static_cast<CssListStyleType>(listStyleType);
  style.colorLuma = in[offset++];
  memcpy(&style.fontFamily, in + offset, sizeof(style.fontFamily));
  offset += sizeof(style.fontFamily);
  style.backgroundLuma = in[offset++];
  style.borderLuma = in[offset++];

  uint32_t definedBits = 0;
  memcpy(&definedBits, in + offset, sizeof(definedBits));
  if ((definedBits & ~CSS_DEFINED_BITS_MASK) != 0) return false;
  style.defined.textAlign = (definedBits & 1 << 0) != 0;
  style.defined.fontStyle = (definedBits & 1 << 1) != 0;
  style.defined.fontWeight = (definedBits & 1 << 2) != 0;
  style.defined.textDecoration = (definedBits & 1 << 3) != 0;
  style.defined.textIndent = (definedBits & 1 << 4) != 0;
  style.defined.marginTop = (definedBits & 1 << 5) != 0;
  style.defined.marginBottom = (definedBits & 1 << 6) != 0;
  style.defined.marginLeft = (definedBits & 1 << 7) != 0;
  style.defined.marginRight = (definedBits & 1 << 8) != 0;
  style.defined.paddingTop = (definedBits & 1 << 9) != 0;
  style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
  style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
  style.defined.paddingRight = (definedBits & 1 << 12) != 0;
  style.defined.imageHeight = (definedBits & 1 << 13) != 0;
  style.defined.imageWidth = (definedBits & 1 << 14) != 0;
  style.defined.display = (definedBits & 1 << 15) != 0;
  style.defined.direction = (definedBits & 1 << 16) != 0;
  style.defined.verticalAlign = (definedBits & 1 << 17) != 0;
  style.defined.listStyleType = (definedBits & 1 << 18) != 0;
  style.defined.fontSize = (definedBits & 1 << 19) != 0;
  style.defined.color = (definedBits & 1 << 20) != 0;
  style.defined.fontFamily = (definedBits & 1 << 21) != 0;
  style.defined.background = (definedBits & 1 << 22) != 0;
  style.defined.border = (definedBits & 1 << 23) != 0;
  style.defined.borderRadius = (definedBits & 1 << 24) != 0;
  return true;
}

// Space-separated tokens of a shorthand value, keeping functions such as rgba(…) and
// url(…) whole.
template <typename Visit>
void forEachShorthandToken(std::string_view value, Visit&& visit) {
  size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && isCssWhitespace(value[i])) ++i;
    const size_t start = i;
    int depth = 0;
    while (i < value.size() && (depth > 0 || !isCssWhitespace(value[i]))) {
      if (value[i] == '(') ++depth;
      if (value[i] == ')' && depth > 0) --depth;
      ++i;
    }
    if (i > start) visit(value.substr(start, i - start));
  }
}

bool parseColorLuma(std::string_view value, uint8_t& luma, float* alpha = nullptr);

// A color over white paper, as the panel shows it: alpha blends toward white.
uint8_t lumaOverPaper(const uint8_t luma, const float alpha) {
  return static_cast<uint8_t>(std::clamp(255.0f - alpha * (255.0f - luma) + 0.5f, 0.0f, 255.0f));
}

// The color part of `background` / `background-color`; images and positions are ignored.
bool parseBackgroundLuma(std::string_view value, uint8_t& luma) {
  bool found = false;
  forEachShorthandToken(stripTrailingImportant(value), [&](std::string_view token) {
    if (found) return;
    if (iequalsAscii(token, "transparent") || iequalsAscii(token, "none")) {
      luma = 255;
      found = true;
      return;
    }
    uint8_t color = 255;
    float alpha = 1.0f;
    if (parseColorLuma(token, color, &alpha)) {
      luma = lumaOverPaper(color, alpha);
      found = true;
    }
  });
  return found;
}

bool borderWidthKeyword(const std::string_view token, CssLength& width) {
  if (iequalsAscii(token, "thin")) width = CssLength{1.0f, CssUnit::Pixels};
  else if (iequalsAscii(token, "medium")) width = CssLength{3.0f, CssUnit::Pixels};
  else if (iequalsAscii(token, "thick")) width = CssLength{5.0f, CssUnit::Pixels};
  else return false;
  return true;
}

// font-size keywords as em of the parent (CSS absolute-size table, medium = 1).
bool fontSizeKeyword(const std::string_view value, CssLength& out) {
  struct Keyword {
    std::string_view name;
    float em;
  };
  static constexpr Keyword KEYWORDS[] = {
      {"xx-small", 0.5625f}, {"x-small", 0.625f}, {"small", 0.8125f}, {"medium", 1.0f}, {"large", 1.125f},
      {"x-large", 1.5f},     {"xx-large", 2.0f},  {"xxx-large", 3.0f}, {"smaller", 0.8333f}, {"larger", 1.2f},
  };
  for (const auto& keyword : KEYWORDS) {
    if (iequalsAscii(value, keyword.name)) {
      out = CssLength{keyword.em, CssUnit::Em};
      return true;
    }
  }
  return false;
}

int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  const char lower = asciiToLower(c);
  if (lower >= 'a' && lower <= 'f') return lower - 'a' + 10;
  return -1;
}

uint8_t lumaOf(const int r, const int g, const int b) {
  // Rec. 601 weights: what the reader perceives as darkness on a gray panel.
  const int luma = (299 * r + 587 * g + 114 * b + 500) / 1000;
  return static_cast<uint8_t>(std::clamp(luma, 0, 255));
}

// CSS color -> luminance. Accepts #rgb, #rgba, #rrggbb, #rrggbbaa, rgb()/rgba() and the named
// colors e-books actually use. False for transparent / inherit / currentColor / unknown.
bool parseColorLuma(std::string_view value, uint8_t& luma, float* alpha) {
  if (alpha) *alpha = 1.0f;
  value = trimCssWhitespace(value);
  if (value.empty()) return false;
  if (value.front() == '#') {
    const std::string_view hex = value.substr(1);
    int rgb[3] = {};
    if (hex.size() == 3 || hex.size() == 4) {
      for (int i = 0; i < 3; ++i) {
        const int d = hexDigit(hex[i]);
        if (d < 0) return false;
        rgb[i] = d * 17;
      }
      if (hex.size() == 4 && alpha && hexDigit(hex[3]) >= 0) *alpha = hexDigit(hex[3]) / 15.0f;
    } else if (hex.size() == 6 || hex.size() == 8) {
      for (int i = 0; i < 3; ++i) {
        const int hi = hexDigit(hex[2 * i]);
        const int lo = hexDigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        rgb[i] = hi * 16 + lo;
      }
      if (hex.size() == 8 && alpha && hexDigit(hex[6]) >= 0 && hexDigit(hex[7]) >= 0)
        *alpha = (hexDigit(hex[6]) * 16 + hexDigit(hex[7])) / 255.0f;
    } else {
      return false;
    }
    luma = lumaOf(rgb[0], rgb[1], rgb[2]);
    return true;
  }
  const size_t open = value.find('(');
  if (open != std::string_view::npos) {
    const std::string_view function = trimCssWhitespace(value.substr(0, open));
    if (!iequalsAscii(function, "rgb") && !iequalsAscii(function, "rgba")) return false;
    const size_t close = value.find(')', open);
    if (close == std::string_view::npos) return false;
    int rgb[3] = {};
    int count = 0;
    bool ok = true;
    forEachDelimitedToken(value.substr(open + 1, close - open - 1),
                          [](const char c) { return c == ',' || c == '/' || isCssWhitespace(c); },
                          [&](std::string_view token) {
                            float number = 0;
                            const bool percent = !token.empty() && token.back() == '%';
                            if (percent) token.remove_suffix(1);
                            if (!tryParseFloat(token, number)) {
                              ok = false;
                              return;
                            }
                            if (count >= 3) {
                              if (count == 3 && alpha) *alpha = std::clamp(percent ? number / 100.0f : number, 0.0f, 1.0f);
                              ++count;
                              return;
                            }
                            rgb[count++] = std::clamp(static_cast<int>(percent ? number * 2.55f : number), 0, 255);
                          });
    if (!ok || count < 3) return false;
    luma = lumaOf(rgb[0], rgb[1], rgb[2]);
    return true;
  }
  struct Named {
    std::string_view name;
    uint8_t r, g, b;
  };
  static constexpr Named NAMED[] = {
      {"black", 0, 0, 0},           {"white", 255, 255, 255},     {"gray", 128, 128, 128},
      {"grey", 128, 128, 128},       {"silver", 192, 192, 192},    {"darkgray", 169, 169, 169},
      {"darkgrey", 169, 169, 169},   {"dimgray", 105, 105, 105},   {"dimgrey", 105, 105, 105},
      {"lightgray", 211, 211, 211},  {"lightgrey", 211, 211, 211}, {"red", 255, 0, 0},
      {"maroon", 128, 0, 0},         {"darkred", 139, 0, 0},       {"brown", 165, 42, 42},
      {"navy", 0, 0, 128},           {"blue", 0, 0, 255},          {"darkblue", 0, 0, 139},
      {"midnightblue", 25, 25, 112}, {"green", 0, 128, 0},         {"darkgreen", 0, 100, 0},
      {"teal", 0, 128, 128},         {"purple", 128, 0, 128},      {"olive", 128, 128, 0},
      {"orange", 255, 165, 0},       {"darkslategray", 47, 79, 79}, {"darkslateblue", 72, 61, 139},
      {"saddlebrown", 139, 69, 19},
  };
  for (const auto& named : NAMED) {
    if (iequalsAscii(value, named.name)) {
      luma = lumaOf(named.r, named.g, named.b);
      return true;
    }
  }
  return false;
}

// url(...) relative to the stylesheet's own path inside the book. Empty for absolute
// schemes (res://, http:, data:) and paths that climb above the book root.
std::string resolveStylesheetUrl(const std::string_view stylesheetPath, std::string_view url) {
  url = trimCssWhitespace(url);
  if (url.size() >= 2 && (url.front() == '"' || url.front() == '\'') && url.back() == url.front()) {
    url = url.substr(1, url.size() - 2);
  }
  const size_t cut = url.find_first_of("?#");
  if (cut != std::string_view::npos) url = url.substr(0, cut);
  if (url.empty() || url.find(':') != std::string_view::npos) return {};
  std::string joined;
  if (url.front() != '/') {
    const size_t slash = stylesheetPath.rfind('/');
    if (slash != std::string_view::npos) joined.assign(stylesheetPath.substr(0, slash + 1));
  }
  joined.append(url.data(), url.size());
  // Collapse "." and ".." segments.
  std::string out;
  size_t start = 0;
  while (start <= joined.size()) {
    size_t end = joined.find('/', start);
    if (end == std::string::npos) end = joined.size();
    const std::string_view segment(joined.data() + start, end - start);
    if (segment == "..") {
      if (out.empty()) return {};
      const size_t previous = out.rfind('/');
      out.erase(previous == std::string::npos ? 0 : previous);
    } else if (!segment.empty() && segment != ".") {
      if (!out.empty()) out.push_back('/');
      out.append(segment.data(), segment.size());
    }
    start = end + 1;
  }
  return out;
}

}  // anonymous namespace

namespace {
bool containsAsciiCaseless(const std::string_view haystack, const std::string_view lowerNeedle) {
  if (lowerNeedle.size() > haystack.size()) return false;
  for (size_t i = 0; i + lowerNeedle.size() <= haystack.size(); ++i) {
    if (iequalsAscii(haystack.substr(i, lowerNeedle.size()), lowerNeedle)) return true;
  }
  return false;
}
}  // namespace

CssParser::FontCategory CssParser::classifyFontName(const std::string_view name) {
  const auto has = [name](const std::string_view needle) {
    return name.find(needle) != std::string_view::npos || containsAsciiCaseless(name, needle);
  };
  // Order matters: 仿宋 contains 宋, and sans-serif contains serif.
  if (has("仿宋") || has("fangsong") || has("fang song")) return FontCategory::FangSong;
  if (has("楷") || has("kai")) return FontCategory::Kai;
  if (has("圆") || has("圓") || has("yuan") || has("round")) return FontCategory::Round;
  if (has("黑") || has("hei") || has("雅黑") || has("yahei") || has("gothic") || has("sans") || has("pingfang") ||
      has("苹方")) {
    return FontCategory::Sans;
  }
  if (has("宋") || has("song") || has("明") || has("ming") || has("serif")) return FontCategory::Serif;
  return FontCategory::None;
}

uint16_t CssParser::internFamily(std::string_view candidates) const {
  candidates = trimCssWhitespace(candidates);
  if (candidates.empty() || candidates.size() > 1024) return 0;
  for (uint16_t i = 0; i < familyCount_; ++i) {
    if (familyAt(i + 1) == candidates) return i + 1;
  }
  if (familyCount_ >= MAX_FONT_FAMILIES || familyCharsUsed_ + candidates.size() > MAX_FAMILY_CHARS) return 0;
  if (!familyOffsets_) {
    familyOffsets_ = makeUniqueNoThrow<uint32_t[]>(MAX_FONT_FAMILIES + 1);
    familyChars_ = makeUniqueNoThrow<char[]>(MAX_FAMILY_CHARS);
    if (!familyOffsets_ || !familyChars_) {
      familyOffsets_.reset();
      familyChars_.reset();
      return 0;
    }
    familyOffsets_[0] = 0;
  }
  memcpy(familyChars_.get() + familyCharsUsed_, candidates.data(), candidates.size());
  familyCharsUsed_ += static_cast<uint32_t>(candidates.size());
  familyOffsets_[++familyCount_] = familyCharsUsed_;
  return familyCount_;
}

std::string_view CssParser::familyAt(const uint16_t index) const {
  if (index == 0 || index > familyCount_ || !familyOffsets_) return {};
  const uint32_t begin = familyOffsets_[index - 1];
  return {familyChars_.get() + begin, familyOffsets_[index] - begin};
}

bool CssParser::addFontFace(std::string_view family, const std::string_view path, const FontCategory category) {
  family = trimCssWhitespace(family);
  if (family.size() >= 2 && (family.front() == '"' || family.front() == '\'') && family.back() == family.front()) {
    family = trimCssWhitespace(family.substr(1, family.size() - 2));
  }
  if (family.empty() || (path.empty() && category == FontCategory::None)) return false;
  for (uint8_t i = 0; i < fontFaceCount_; ++i) {
    // Repeats of one family are kept in order: the reader takes the first whose file exists
    // (books often declare a name twice, the first time for a file they never shipped).
    const FontFace existing = fontFaceAt(i);
    if (existing.family == family && existing.path == path && existing.category == category) return false;
  }
  if (fontFaceCount_ >= MAX_FONT_FACES || family.size() > UINT16_MAX || path.size() > UINT16_MAX ||
      faceCharsUsed_ + family.size() + path.size() > MAX_FACE_CHARS) {
    return false;
  }
  if (!faces_) {
    faces_ = makeUniqueNoThrow<FaceRecord[]>(MAX_FONT_FACES);
    faceChars_ = makeUniqueNoThrow<char[]>(MAX_FACE_CHARS);
    if (!faces_ || !faceChars_) {
      faces_.reset();
      faceChars_.reset();
      return false;
    }
  }
  FaceRecord& record = faces_[fontFaceCount_++];
  record.familyOff = static_cast<uint16_t>(faceCharsUsed_);
  record.familyLen = static_cast<uint16_t>(family.size());
  memcpy(faceChars_.get() + faceCharsUsed_, family.data(), family.size());
  faceCharsUsed_ += static_cast<uint32_t>(family.size());
  record.pathOff = static_cast<uint16_t>(faceCharsUsed_);
  record.pathLen = static_cast<uint16_t>(path.size());
  memcpy(faceChars_.get() + faceCharsUsed_, path.data(), path.size());
  faceCharsUsed_ += static_cast<uint32_t>(path.size());
  record.category = category;
  return true;
}

CssParser::FontFace CssParser::fontFaceAt(const uint8_t index) const {
  if (index >= fontFaceCount_ || !faces_) return {};
  const FaceRecord& record = faces_[index];
  return {std::string_view(faceChars_.get() + record.familyOff, record.familyLen),
          std::string_view(faceChars_.get() + record.pathOff, record.pathLen), record.category};
}

int CssParser::compareEntryToPieces(const SelectorEntry& entry, const std::string_view p0, const std::string_view p1,
                                    const std::string_view p2) const {
  const char* stored = selectorPool_.get() + entry.offset;
  const std::string_view pieces[] = {p0, p1, p2};
  size_t index = 0;
  for (const std::string_view piece : pieces) {
    for (const char c : piece) {
      if (index == entry.length) return -1;
      const auto storedByte = static_cast<unsigned char>(stored[index]);
      const auto probeByte = static_cast<unsigned char>(asciiToLower(c));
      if (storedByte != probeByte) return storedByte < probeByte ? -1 : 1;
      ++index;
    }
  }
  return index == entry.length ? 0 : 1;
}

size_t CssParser::lowerBound(const std::string_view p0, const std::string_view p1, const std::string_view p2,
                             bool& exact) const {
  size_t low = 0;
  size_t high = entryCount_;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (compareEntryToPieces(entries_[middle], p0, p1, p2) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  exact = low < entryCount_ && compareEntryToPieces(entries_[low], p0, p1, p2) == 0;
  return low;
}

const CssStyle* CssParser::findStyle(const std::string_view p0, const std::string_view p1,
                                     const std::string_view p2) const {
  bool exact = false;
  const size_t index = lowerBound(p0, p1, p2, exact);
  return exact ? &stylePool_[entries_[index].styleIndex] : nullptr;
}

std::string_view CssParser::selectorAt(const size_t index) const {
  const SelectorEntry& entry = entries_[index];
  return {selectorPool_.get() + entry.offset, entry.length};
}

CssParser::PoolResult CssParser::ensureEntryCapacity(const size_t needed) {
  if (needed <= entryCapacity_) return PoolResult::Ready;
  if (needed > MAX_RULES) return PoolResult::Limit;

  size_t capacity = entryCapacity_ ? entryCapacity_ * 2u : 128u;
  while (capacity < needed) capacity *= 2u;
  capacity = std::min(capacity, MAX_RULES);
  auto grown = makeUniqueNoThrow<SelectorEntry[]>(capacity);
  if (!grown) {
    LOG_ERR("CSS", "OOM: selector index (%zu entries)", capacity);
    return PoolResult::OutOfMemory;
  }
  if (entryCount_ > 0) memcpy(grown.get(), entries_.get(), entryCount_ * sizeof(SelectorEntry));
  entries_ = std::move(grown);
  entryCapacity_ = static_cast<uint16_t>(capacity);
  return PoolResult::Ready;
}

CssParser::PoolResult CssParser::ensureSelectorPoolCapacity(const size_t needed) {
  if (needed <= selectorPoolCapacity_) return PoolResult::Ready;
  if (needed > SELECTOR_POOL_CAP) return PoolResult::Limit;

  size_t capacity = selectorPoolCapacity_ ? selectorPoolCapacity_ * 2u : 4096u;
  while (capacity < needed) capacity *= 2u;
  capacity = std::min(capacity, SELECTOR_POOL_CAP);
  auto grown = makeUniqueNoThrow<char[]>(capacity);
  if (!grown) {
    LOG_ERR("CSS", "OOM: selector pool (%zu bytes)", capacity);
    return PoolResult::OutOfMemory;
  }
  if (selectorPoolSize_ > 0) memcpy(grown.get(), selectorPool_.get(), selectorPoolSize_);
  selectorPool_ = std::move(grown);
  selectorPoolCapacity_ = static_cast<uint32_t>(capacity);
  return PoolResult::Ready;
}

CssParser::PoolResult CssParser::ensureStyleCapacity(const size_t needed) {
  if (needed <= styleCapacity_) return PoolResult::Ready;
  if (needed > MAX_UNIQUE_STYLES) return PoolResult::Limit;

  size_t capacity = styleCapacity_ ? styleCapacity_ * 2u : 16u;
  while (capacity < needed) capacity *= 2u;
  capacity = std::min(capacity, MAX_UNIQUE_STYLES);
  auto grownStyles = makeUniqueNoThrow<CssStyle[]>(capacity);
  if (!grownStyles) {
    LOG_ERR("CSS", "OOM: style pool (%zu styles)", capacity);
    return PoolResult::OutOfMemory;
  }
  for (size_t i = 0; i < styleCount_; ++i) grownStyles[i] = stylePool_[i];
  stylePool_ = std::move(grownStyles);
  styleCapacity_ = static_cast<uint16_t>(capacity);
  return PoolResult::Ready;
}

CssParser::PoolResult CssParser::internStyle(const CssStyle& style, uint16_t& indexOut) {
  uint8_t wire[STYLE_WIRE_BYTES];
  encodeStyleWire(style, wire);
  for (uint16_t i = 0; i < styleCount_; ++i) {
    uint8_t existingWire[STYLE_WIRE_BYTES];
    encodeStyleWire(stylePool_[i], existingWire);
    if (memcmp(existingWire, wire, STYLE_WIRE_BYTES) == 0) {
      indexOut = i;
      return PoolResult::Ready;
    }
  }

  const PoolResult capacityResult = ensureStyleCapacity(static_cast<size_t>(styleCount_) + 1);
  if (capacityResult != PoolResult::Ready) return capacityResult;
  stylePool_[styleCount_] = style;
  indexOut = styleCount_++;
  return PoolResult::Ready;
}

CssParser::RuleInsertResult CssParser::insertOrMerge(const std::string_view selector, const CssStyle& style) {
  descendantIndexDirty_ = true;  // inserts shift store indices
  bool exact = false;
  const size_t position = lowerBound(selector, {}, {}, exact);
  if (exact) {
    const uint16_t currentStyleIndex = entries_[position].styleIndex;
    CssStyle merged = stylePool_[currentStyleIndex];
    merged.applyOver(style);

    bool styleIsShared = false;
    for (uint16_t i = 0; i < entryCount_; ++i) {
      if (i != position && entries_[i].styleIndex == currentStyleIndex) {
        styleIsShared = true;
        break;
      }
    }
    if (!styleIsShared) {
      stylePool_[currentStyleIndex] = merged;
      return RuleInsertResult::Merged;
    }

    uint16_t styleIndex = 0;
    const PoolResult result = internStyle(merged, styleIndex);
    if (result == PoolResult::Limit) return RuleInsertResult::Limit;
    if (result == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;
    entries_[position].styleIndex = styleIndex;
    return RuleInsertResult::Merged;
  }

  const PoolResult entryResult = ensureEntryCapacity(static_cast<size_t>(entryCount_) + 1);
  if (entryResult == PoolResult::Limit) return RuleInsertResult::Limit;
  if (entryResult == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;

  const size_t requiredSelectorBytes = static_cast<size_t>(selectorPoolSize_) + selector.size();
  const PoolResult selectorResult = ensureSelectorPoolCapacity(requiredSelectorBytes);
  if (selectorResult == PoolResult::Limit) return RuleInsertResult::Limit;
  if (selectorResult == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;

  uint16_t styleIndex = 0;
  const PoolResult styleResult = internStyle(style, styleIndex);
  if (styleResult == PoolResult::Limit) return RuleInsertResult::Limit;
  if (styleResult == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;

  const uint32_t selectorOffset = selectorPoolSize_;
  char* destination = selectorPool_.get() + selectorOffset;
  for (const char c : selector) *destination++ = asciiToLower(c);
  selectorPoolSize_ = static_cast<uint32_t>(requiredSelectorBytes);

  SelectorEntry* entries = entries_.get();
  memmove(entries + position + 1, entries + position, (entryCount_ - position) * sizeof(SelectorEntry));
  entries[position] = {selectorOffset, styleIndex, static_cast<uint16_t>(selector.size())};
  ++entryCount_;
  return RuleInsertResult::Inserted;
}

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "left") || iequalsAscii(val, "start")) return CssTextAlign::Left;
  if (iequalsAscii(val, "right") || iequalsAscii(val, "end")) return CssTextAlign::Right;
  if (iequalsAscii(val, "center")) return CssTextAlign::Center;
  if (iequalsAscii(val, "justify")) return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "italic") || iequalsAscii(val, "oblique")) return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(std::string_view val) {
  val = trimCssWhitespace(val);

  // Named values
  if (iequalsAscii(val, "bold") || iequalsAscii(val, "bolder")) return CssFontWeight::Bold;
  if (iequalsAscii(val, "normal") || iequalsAscii(val, "lighter")) return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  long numericWeight = 0;
  if (tryParseNumber(val, numericWeight)) {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }
  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(std::string_view val) {
  // text-decoration can have multiple space-separated values. Compare whole tokens
  // so malformed values like "notunderline" do not accidentally enable a line.
  CssTextDecoration result = CssTextDecoration::None;
  bool explicitNone = false;
  forEachDelimitedToken(val, isCssWhitespace, [&](const std::string_view token) {
    if (iequalsAscii(token, "none")) {
      explicitNone = true;
    } else if (iequalsAscii(token, "underline")) {
      result = result | CssTextDecoration::Underline;
    } else if (iequalsAscii(token, "line-through")) {
      result = result | CssTextDecoration::LineThrough;
    }
  });
  return explicitNone ? CssTextDecoration::None : result;
}

CssLength CssParser::interpretLength(std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);
  if (val.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    const char c = val[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  float numericValue;
  if (!tryParseFloat(val.substr(0, unitStart), numericValue)) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  const std::string_view unitPart = val.substr(unitStart);
  auto unit = CssUnit::Pixels;
  if (iequalsAscii(unitPart, "em")) {
    unit = CssUnit::Em;
  } else if (iequalsAscii(unitPart, "rem")) {
    unit = CssUnit::Rem;
  } else if (iequalsAscii(unitPart, "pt")) {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(std::string_view decl, CssStyle& style, const CssParser* owner) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  std::string_view value = trimCssWhitespace(decl.substr(colonPos + 1));

  if (name.empty() || value.empty()) return;

  value = stripTrailingImportant(value);

  if (iequalsAscii(name, "text-align")) {
    style.textAlign = interpretAlignment(value);
    style.defined.textAlign = 1;
  } else if (iequalsAscii(name, "font-style")) {
    style.fontStyle = interpretFontStyle(value);
    style.defined.fontStyle = 1;
  } else if (iequalsAscii(name, "font-weight")) {
    style.fontWeight = interpretFontWeight(value);
    style.defined.fontWeight = 1;
  } else if (iequalsAscii(name, "text-decoration") || iequalsAscii(name, "text-decoration-line")) {
    style.textDecoration = interpretDecoration(value);
    style.defined.textDecoration = 1;
  } else if (iequalsAscii(name, "text-indent")) {
    style.textIndent = interpretLength(value);
    style.defined.textIndent = 1;
  } else if (iequalsAscii(name, "margin-top")) {
    style.marginTop = interpretLength(value);
    style.defined.marginTop = 1;
  } else if (iequalsAscii(name, "margin-bottom")) {
    style.marginBottom = interpretLength(value);
    style.defined.marginBottom = 1;
  } else if (iequalsAscii(name, "margin-left")) {
    style.marginLeft = interpretLength(value);
    style.defined.marginLeft = 1;
  } else if (iequalsAscii(name, "margin-right")) {
    style.marginRight = interpretLength(value);
    style.defined.marginRight = 1;
  } else if (iequalsAscii(name, "margin")) {
    std::string_view margins[4];
    const size_t count = collectEdgeValueTokens(value, margins);
    if (count > 0) {
      style.marginTop = interpretLength(margins[0]);
      style.marginRight = count >= 2 ? interpretLength(margins[1]) : style.marginTop;
      style.marginBottom = count >= 3 ? interpretLength(margins[2]) : style.marginTop;
      style.marginLeft = count >= 4 ? interpretLength(margins[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (iequalsAscii(name, "padding-top")) {
    style.paddingTop = interpretLength(value);
    style.defined.paddingTop = 1;
  } else if (iequalsAscii(name, "padding-bottom")) {
    style.paddingBottom = interpretLength(value);
    style.defined.paddingBottom = 1;
  } else if (iequalsAscii(name, "padding-left")) {
    style.paddingLeft = interpretLength(value);
    style.defined.paddingLeft = 1;
  } else if (iequalsAscii(name, "padding-right")) {
    style.paddingRight = interpretLength(value);
    style.defined.paddingRight = 1;
  } else if (iequalsAscii(name, "padding")) {
    std::string_view paddings[4];
    const size_t count = collectEdgeValueTokens(value, paddings);
    if (count > 0) {
      style.paddingTop = interpretLength(paddings[0]);
      style.paddingRight = count >= 2 ? interpretLength(paddings[1]) : style.paddingTop;
      style.paddingBottom = count >= 3 ? interpretLength(paddings[2]) : style.paddingTop;
      style.paddingLeft = count >= 4 ? interpretLength(paddings[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (iequalsAscii(name, "height")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (iequalsAscii(name, "width")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (iequalsAscii(name, "display")) {
    style.display = iequalsAscii(value, "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "direction")) {
    if (iequalsAscii(value, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(value, "ltr")) {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (iequalsAscii(name, "vertical-align")) {
    if (iequalsAscii(value, "super")) {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (iequalsAscii(value, "sub")) {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    }
  } else if (iequalsAscii(name, "font-size")) {
    CssLength size;
    if (fontSizeKeyword(value, size) || (tryInterpretLength(value, size) && size.value > 0)) {
      style.fontSize = size;
      style.defined.fontSize = 1;
    }
  } else if (iequalsAscii(name, "color")) {
    uint8_t luma = 0;
    if (parseColorLuma(value, luma)) {
      style.colorLuma = luma;
      style.defined.color = 1;
    }
  } else if (iequalsAscii(name, "background") || iequalsAscii(name, "background-color")) {
    uint8_t luma = 255;
    if (parseBackgroundLuma(value, luma)) {
      style.backgroundLuma = luma;
      style.defined.background = 1;
    }
  } else if (iequalsAscii(name, "border")) {
    // width, style and color in any order; only a uniform border is drawn.
    CssLength width{3.0f, CssUnit::Pixels};  // CSS medium when only a style is given
    uint8_t luma = style.hasColor() ? style.colorLuma : 0;
    bool visible = false;
    forEachShorthandToken(stripTrailingImportant(value), [&](std::string_view token) {
      CssLength length;
      uint8_t color = 0;
      float alpha = 1.0f;
      if (borderWidthKeyword(token, width)) return;
      if (iequalsAscii(token, "none") || iequalsAscii(token, "hidden")) {
        visible = false;
        width = CssLength{};
        return;
      }
      if (iequalsAscii(token, "solid") || iequalsAscii(token, "dotted") || iequalsAscii(token, "dashed") ||
          iequalsAscii(token, "double") || iequalsAscii(token, "groove") || iequalsAscii(token, "ridge") ||
          iequalsAscii(token, "inset") || iequalsAscii(token, "outset")) {
        visible = true;
        return;
      }
      if (tryInterpretLength(token, length)) {
        width = length;
        return;
      }
      if (parseColorLuma(token, color, &alpha)) luma = lumaOverPaper(color, alpha);
    });
    style.borderWidth = visible ? width : CssLength{};
    style.borderLuma = luma;
    style.defined.border = 1;
  } else if (iequalsAscii(name, "border-width")) {
    std::string_view widths[4];
    const size_t count = collectEdgeValueTokens(value, widths);
    CssLength first;
    bool uniform = count > 0 && (borderWidthKeyword(widths[0], first) || tryInterpretLength(widths[0], first));
    for (size_t i = 1; uniform && i < count; ++i) {
      CssLength other;
      uniform = (borderWidthKeyword(widths[i], other) || tryInterpretLength(widths[i], other)) &&
                other.value == first.value && other.unit == first.unit;
    }
    // Per-side widths (a rule above or below) are not panels: no border.
    style.borderWidth = uniform ? first : CssLength{};
    if (!style.hasBorder()) style.borderLuma = style.hasColor() ? style.colorLuma : 0;
    style.defined.border = 1;
  } else if (iequalsAscii(name, "border-color")) {
    uint8_t color = 0;
    float alpha = 1.0f;
    std::string_view colors[4];
    if (collectEdgeValueTokens(value, colors) > 0 && parseColorLuma(colors[0], color, &alpha)) {
      // A color alone draws nothing (the style defaults to none); it only tints a border.
      style.borderLuma = lumaOverPaper(color, alpha);
      if (!style.hasBorder()) style.borderWidth = CssLength{};
      style.defined.border = 1;
    }
  } else if (iequalsAscii(name, "border-style")) {
    const std::string_view kind = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(kind, "none") || iequalsAscii(kind, "hidden")) {
      style.borderWidth = CssLength{};
      style.defined.border = 1;
    } else if (!kind.empty() && !style.hasBorder()) {
      style.borderWidth = CssLength{3.0f, CssUnit::Pixels};  // medium
      style.defined.border = 1;
    }
  } else if (iequalsAscii(name, "border-radius")) {
    std::string_view radii[4];
    CssLength radius;
    if (collectEdgeValueTokens(value, radii) > 0 && tryInterpretLength(radii[0], radius)) {
      style.borderRadius = radius;
      style.defined.borderRadius = 1;
    }
  } else if (iequalsAscii(name, "font-family")) {
    if (owner) {
      const uint16_t family = owner->internFamily(value);
      if (family != 0) {
        style.fontFamily = family;
        style.defined.fontFamily = 1;
      }
    }
  } else if (iequalsAscii(name, "list-style-type")) {
    const std::string_view listStyleValue = stripTrailingImportant(value);
    style.listStyleType = iequalsAscii(listStyleValue, "none") ? CssListStyleType::None : CssListStyleType::Disc;
    style.defined.listStyleType = 1;
  }
}

CssStyle CssParser::parseDeclarations(std::string_view declBlock, const CssParser* owner) {
  CssStyle style;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        parseDeclarationIntoStyle(declBlock.substr(start, i - start), style, owner);
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style) {
  // Skip rules that don't define any supported properties to save RAM.
  if (!style.defined.anySet()) {
    return;
  }

  // Walk comma-separated selectors in place. The bounded store reports every
  // capacity or allocation failure without crossing a throwing STL boundary.
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // TODO: Support richer CSS selector syntax in the future. For now we only
        // handle `tag`, `.class`, or `tag.class`. Reject anything containing a
        // character that introduces unsupported syntax:
        //   '+'  adjacent sibling combinator
        //   '>'  child combinator
        //   '['  attribute selector
        //   ':'  pseudo class/element
        //   '#'  ID selector
        //   '~'  general sibling combinator
        //   '*'  wildcard
        //   ' '  descendant combinator
        // Single-pass scan via find_first_of instead of eight sequential find() calls.
        // Descendant (' ') and child ('>') combinators between simple compounds are kept:
        // book stylesheets style "div.bt p" or "h2.mm b". '>' is matched as a descendant.
        constexpr std::string_view kUnsupportedSelectorChars = "+[:#~*";
        if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) return;
        char normalized[MAX_SELECTOR_LENGTH];
        size_t normalizedLen = 0;
        bool pendingSpace = false;
        for (const char c : sel) {
          if (isCssWhitespace(c) || c == '>') {
            pendingSpace = normalizedLen > 0;
            continue;
          }
          if (pendingSpace) normalized[normalizedLen++] = ' ';
          pendingSpace = false;
          normalized[normalizedLen++] = c;
        }
        if (normalizedLen == 0) return;
        sel = std::string_view(normalized, normalizedLen);
        if (sel.find(' ') != std::string_view::npos) descendantIndexDirty_ = true;

        if (ruleGrowthStopped_) {
          // Continue the cascade for stored selectors without retrying failed
          // allocations for new rules.
          bool exact = false;
          const size_t matchingIndex = lowerBound(sel, {}, {}, exact);
          if (!exact || matchingIndex >= entryCount_) return;
        }
        const RuleInsertResult result = insertOrMerge(sel, style);
        if (result == RuleInsertResult::Limit) {
          LOG_ERR("CSS", "CSS rule store limit reached at %u rules", entryCount_);
          ruleGrowthStopped_ = true;
        } else if (result == RuleInsertResult::OutOfMemory) {
          LOG_ERR("CSS", "OOM while growing CSS rule store at %u rules", entryCount_);
          ruleGrowthStopped_ = true;
        }
      });
}

// Main parsing entry point

CssParser::ParseResult CssParser::loadFromStream(HalFile& source, const std::string_view stylesheetPath) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return ParseResult::Error;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  // @font-face, read as it streams: src lists run past 1 KB (a dozen local() names per
  // face), so each comma-separated source is handled on its own instead of buffered whole.
  enum class FaceDecl : uint8_t { None, Family, Src, Other };
  struct FontFaceReader {
    char keyword[16];
    size_t keywordLen = 0;
    bool keywordDone = false;
    bool active = false;
    char name[24];
    size_t nameLen = 0;
    bool inValue = false;
    FaceDecl decl = FaceDecl::None;
    char part[256];
    size_t partLen = 0;
    bool partOverflow = false;
    char quote = 0;
    int parens = 0;
    char family[96];
    size_t familyLen = 0;
    std::string path;
    FontCategory category = FontCategory::None;
  };
  auto face = makeUniqueNoThrow<FontFaceReader>();  // ~400 B kept off the parse stack
  const auto resetFace = [&face]() {
    if (!face) return;
    face->keywordLen = 0;
    face->keywordDone = false;
    face->active = false;
    face->nameLen = 0;
    face->inValue = false;
    face->decl = FaceDecl::None;
    face->partLen = 0;
    face->partOverflow = false;
    face->quote = 0;
    face->parens = 0;
    face->familyLen = 0;
    face->path.clear();
    face->category = FontCategory::None;
  };
  const auto endFacePart = [&]() {
    if (!face || face->partOverflow) {
      if (face) face->partLen = 0, face->partOverflow = false;
      return;
    }
    const std::string_view part = trimCssWhitespace(std::string_view(face->part, face->partLen));
    face->partLen = 0;
    if (part.empty()) return;
    if (face->decl == FaceDecl::Family) {
      const size_t n = std::min(part.size(), sizeof(face->family));
      memcpy(face->family, part.data(), n);
      face->familyLen = n;
    } else if (face->decl == FaceDecl::Src) {
      const size_t open = part.find('(');
      const size_t close = part.rfind(')');
      if (open == std::string_view::npos || close == std::string_view::npos || close < open) return;
      const std::string_view function = trimCssWhitespace(part.substr(0, open));
      const std::string_view argument = part.substr(open + 1, close - open - 1);
      if (iequalsAscii(function, "url") && face->path.empty() && !stylesheetPath.empty()) {
        face->path = resolveStylesheetUrl(stylesheetPath, argument);
      } else if (iequalsAscii(function, "local") && face->category == FontCategory::None) {
        face->category = classifyFontName(argument);
      }
    }
  };
  const auto faceChar = [&](const char c) {
    if (!face) return;
    const auto push = [&](char ch) {
      if (face->partLen < sizeof(face->part)) {
        face->part[face->partLen++] = ch;
      } else {
        face->partOverflow = true;
      }
    };
    if (!face->inValue) {
      if (c == ':') {
        const std::string_view name = trimCssWhitespace(std::string_view(face->name, face->nameLen));
        face->decl = iequalsAscii(name, "font-family") ? FaceDecl::Family
                     : iequalsAscii(name, "src")        ? FaceDecl::Src
                                                        : FaceDecl::Other;
        face->inValue = true;
        face->partLen = 0;
        face->partOverflow = false;
      } else if (c == ';') {
        face->nameLen = 0;
      } else if (face->nameLen < sizeof(face->name)) {
        face->name[face->nameLen++] = c;
      }
      return;
    }
    if (face->quote) {
      if (c == face->quote) face->quote = 0;
      push(c);
      return;
    }
    if (c == '"' || c == '\'') {
      face->quote = c;
      push(c);
      return;
    }
    if (c == '(') ++face->parens;
    if (c == ')' && face->parens > 0) --face->parens;
    if (face->parens == 0 && (c == ';' || (c == ',' && face->decl == FaceDecl::Src))) {
      endFacePart();
      if (c == ';') {
        face->inValue = false;
        face->nameLen = 0;
        face->decl = FaceDecl::None;
      }
      return;
    }
    push(c);
  };
  const auto finishFace = [&]() {
    if (!face) return;
    if (face->inValue) endFacePart();
    addFontFace(std::string_view(face->family, face->familyLen), face->path, face->category);
    resetFace();
  };

  int bodyDepth = 0;
  bool skippingRule = false;
  bool selectorTruncated = false;
  bool declarationTruncated = false;
  bool inputTruncated = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (face && !face->keywordDone) {
        if (c != '{' && c != ';' && !isCssWhitespace(c)) {
          if (face->keywordLen < sizeof(face->keyword)) face->keyword[face->keywordLen++] = c;
          return;
        }
        face->keywordDone = true;
        face->active = iequalsAscii(std::string_view(face->keyword, face->keywordLen), "font-face");
        if (isCssWhitespace(c)) return;
      }
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) {
          if (face && face->active) finishFace();
          inAtRule = false;
        }
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      } else if (face && face->active && atDepth == 1) {
        faceChar(c);
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        resetFace();
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        skippingRule = selectorTruncated || selector.size() > MAX_SELECTOR_LENGTH * 4;
        return;
      }
      if (!selector.push_back(c)) {
        selectorTruncated = true;
        inputTruncated = true;
      }
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declarationTruncated && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle, this);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector, currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        selectorTruncated = false;
        declarationTruncated = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declarationTruncated && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle, this);
        }
        declBuffer.clear();
        declarationTruncated = false;
      } else {
        if (!declBuffer.push_back(c)) {
          declarationTruncated = true;
          inputTruncated = true;
        }
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  if (inputTruncated) {
    LOG_ERR("CSS", "CSS input exceeded parser buffer; cache will remain partial");
  }
  const bool incompleteInput = bodyDepth > 0 || inAtRule || inComment || !selector.empty();
  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", ruleCount(), totalRead);
  return ruleGrowthStopped_ || inputTruncated || incompleteInput ? ParseResult::Partial : ParseResult::Complete;
}

// Style resolution

void CssParser::indexDescendantRules() const {
  descendantIndexDirty_ = false;
  descendantCount_ = 0;
  for (uint16_t i = 0; i < entryCount_; ++i) {
    if (selectorAt(i).find(' ') == std::string_view::npos) continue;
    if (!descendantRules_) {
      descendantRules_ = makeUniqueNoThrow<uint16_t[]>(MAX_DESCENDANT_RULES);
      if (!descendantRules_) return;
    }
    if (descendantCount_ >= MAX_DESCENDANT_RULES) return;
    descendantRules_[descendantCount_++] = i;
  }
}

bool CssParser::hasDescendantRules() const {
  if (descendantIndexDirty_) indexDescendantRules();
  return descendantCount_ > 0;
}

namespace {
bool hasClass(const std::string_view classes, const std::string_view wanted) {
  bool found = false;
  forEachDelimitedToken(classes, isCssWhitespace, [&](std::string_view cls) { found = found || cls == wanted; });
  return found;
}

bool equalsCaseless(const std::string_view a, const std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return asciiToLower(x) == asciiToLower(y); });
}

// One compound: "tag", ".class" or "tag.class" (all classes of a.b.c must be present).
bool compoundMatches(const std::string_view compound, const std::string_view tag, const std::string_view classes) {
  const size_t dot = compound.find('.');
  const std::string_view compoundTag = compound.substr(0, dot);
  if (!compoundTag.empty() && !equalsCaseless(compoundTag, tag)) return false;
  if (dot == std::string_view::npos) return true;
  std::string_view rest = compound.substr(dot + 1);
  while (true) {
    const size_t next = rest.find('.');
    if (!hasClass(classes, rest.substr(0, next))) return false;
    if (next == std::string_view::npos) return true;
    rest = rest.substr(next + 1);
  }
}
}  // namespace

CssStyle CssParser::resolveStyle(const std::string_view tagName, const std::string_view classAttr,
                                 const ElementRef* ancestors, const size_t ancestorCount) const {
  CssStyle result = resolveStyle(tagName, classAttr);
  if (!hasDescendantRules()) return result;
  for (uint16_t r = 0; r < descendantCount_; ++r) {
    const uint16_t entry = descendantRules_[r];
    std::string_view selector = selectorAt(entry);
    // Rightmost compound must be this element; then walk outward through the ancestors.
    size_t space = selector.rfind(' ');
    if (!compoundMatches(selector.substr(space + 1), tagName, classAttr)) continue;
    selector = selector.substr(0, space);
    size_t next = 0;
    bool matched = true;
    while (true) {
      space = selector.rfind(' ');
      const std::string_view compound = space == std::string_view::npos ? selector : selector.substr(space + 1);
      while (next < ancestorCount && !compoundMatches(compound, ancestors[next].tag, ancestors[next].classes)) ++next;
      if (next >= ancestorCount) {
        matched = false;
        break;
      }
      ++next;
      if (space == std::string_view::npos) break;
      selector = selector.substr(0, space);
    }
    if (matched) result.applyOver(stylePool_[entries_[entry].styleIndex]);
  }
  return result;
}

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr) const {
  CssStyle result;

  // 1. Apply element-level style (lowest priority).
  if (const CssStyle* style = findStyle(tagName)) {
    result.applyOver(*style);
  }

  if (classAttr.empty()) return result;

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (const CssStyle* style = findStyle(".", cls)) {
      result.applyOver(*style);
    }
  });

  // TODO: Support combinations of classes (e.g. style on p.class1.class2)
  // 3. Apply element.class styles (higher priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (const CssStyle* style = findStyle(tagName, ".", cls)) {
      result.applyOver(*style);
    }
  });

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

CssStyle CssParser::parseInlineStyleWithFamilies(std::string_view styleValue) const {
  return parseDeclarations(styleValue, this);
}

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char rulesCacheTmp[] = "/css_rules.cache.tmp";
constexpr char rulesCacheBackup[] = "/css_rules.cache.bak";
constexpr uint8_t CSS_CACHE_FLAG_PARTIAL = 1 << 0;
constexpr uint8_t CSS_CACHE_KNOWN_FLAGS = CSS_CACHE_FLAG_PARTIAL;

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

bool CssParser::restoreCacheBackupIfNeeded() const {
  if (cachePath.empty()) {
    return false;
  }

  const std::string finalPath = cachePath + rulesCache;
  if (Storage.exists(finalPath.c_str())) {
    return true;
  }

  const std::string backupPath = cachePath + rulesCacheBackup;
  if (!Storage.exists(backupPath.c_str())) {
    return false;
  }

  if (!Storage.rename(backupPath.c_str(), finalPath.c_str())) {
    LOG_ERR("CSS", "Failed to restore CSS cache backup");
    return false;
  }

  LOG_DBG("CSS", "Restored CSS cache backup after interrupted replacement");
  return true;
}

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
  Storage.remove((cachePath + rulesCacheTmp).c_str());
  Storage.remove((cachePath + rulesCacheBackup).c_str());
}

CssParser::CacheStatus CssParser::inspectCache() const {
  if (cachePath.empty() || (!hasCache() && !restoreCacheBackupIfNeeded())) {
    return CacheStatus::Missing;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return CacheStatus::Invalid;
  }

  uint8_t version = 0;
  uint8_t flags = 0;
  uint16_t ruleCount = 0;
  if (file.read(&version, sizeof(version)) != sizeof(version) || version != CSS_CACHE_VERSION ||
      file.read(&flags, sizeof(flags)) != sizeof(flags) || (flags & ~CSS_CACHE_KNOWN_FLAGS) != 0 ||
      file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount) || ruleCount > MAX_RULES) {
    return CacheStatus::Invalid;
  }

  const bool partial = (flags & CSS_CACHE_FLAG_PARTIAL) != 0;
  if (!partial) {
    // Complete caches are fully validated while hydrating, avoiding a second
    // payload scan on every EPUB open.
    return CacheStatus::Complete;
  }

  const auto skipBytes = [&file](const size_t byteCount) {
    return static_cast<size_t>(file.available()) >= byteCount && file.seekCur(byteCount);
  };
  size_t selectorBytes = 0;
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen) || selectorLen == 0 ||
        selectorLen > MAX_SELECTOR_LENGTH) {
      return CacheStatus::Invalid;
    }
    selectorBytes += selectorLen;
    if (selectorBytes > SELECTOR_POOL_CAP || !skipBytes(static_cast<size_t>(selectorLen) + STYLE_WIRE_BYTES)) {
      return CacheStatus::Invalid;
    }
  }

  uint16_t familyCount = 0;
  if (file.read(&familyCount, sizeof(familyCount)) != sizeof(familyCount) || familyCount > MAX_FONT_FAMILIES) {
    return CacheStatus::Invalid;
  }
  for (uint16_t i = 0; i < familyCount; ++i) {
    uint16_t length = 0;
    if (file.read(&length, sizeof(length)) != sizeof(length) || !skipBytes(length)) return CacheStatus::Invalid;
  }
  uint8_t faceCount = 0;
  if (file.read(&faceCount, sizeof(faceCount)) != sizeof(faceCount) || faceCount > MAX_FONT_FACES) {
    return CacheStatus::Invalid;
  }
  for (uint8_t i = 0; i < faceCount; ++i) {
    for (int part = 0; part < 2; ++part) {
      uint16_t length = 0;
      if (file.read(&length, sizeof(length)) != sizeof(length) || !skipBytes(length)) return CacheStatus::Invalid;
    }
    if (!skipBytes(1)) return CacheStatus::Invalid;
  }

  if (file.available() != 0) {
    return CacheStatus::Invalid;
  }

  return CacheStatus::Partial;
}

bool CssParser::saveToCache(const bool complete) const {
  if (cachePath.empty()) {
    return false;
  }

  const std::string finalPath = cachePath + rulesCache;
  const std::string tmpPath = cachePath + rulesCacheTmp;
  const std::string backupPath = cachePath + rulesCacheBackup;

  Storage.remove(tmpPath.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("CSS", tmpPath, file)) {
    return false;
  }

  bool writeOk = true;
  const auto writeBytes = [&file, &writeOk](const void* data, const size_t size) {
    if (writeOk && size > 0 && file.write(data, size) != size) {
      writeOk = false;
    }
  };
  const auto writeByte = [&writeBytes](const uint8_t value) { writeBytes(&value, sizeof(value)); };

  writeByte(CssParser::CSS_CACHE_VERSION);

  // A partial cache can style the current low-memory session, but the next
  // EPUB load must retry the source stylesheets instead of trusting it.
  writeByte(complete ? 0 : CSS_CACHE_FLAG_PARTIAL);

  // Write rule count
  const uint16_t ruleCount = entryCount_;
  writeBytes(&ruleCount, sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (uint16_t i = 0; i < entryCount_; ++i) {
    const std::string_view selector = selectorAt(i);
    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(selector.size());
    writeBytes(&selectorLen, sizeof(selectorLen));
    writeBytes(selector.data(), selectorLen);

    uint8_t styleWire[STYLE_WIRE_BYTES];
    encodeStyleWire(stylePool_[entries_[i].styleIndex], styleWire);
    writeBytes(styleWire, sizeof(styleWire));
    if (!writeOk) break;
  }

  // font-family candidate lists (rules hold 1-based indices into them), then @font-face.
  writeBytes(&familyCount_, sizeof(familyCount_));
  for (uint16_t i = 1; i <= familyCount_ && writeOk; ++i) {
    const std::string_view family = familyAt(i);
    const auto length = static_cast<uint16_t>(family.size());
    writeBytes(&length, sizeof(length));
    writeBytes(family.data(), length);
  }
  writeByte(fontFaceCount_);
  for (uint8_t i = 0; i < fontFaceCount_ && writeOk; ++i) {
    const FontFace fontFace = fontFaceAt(i);
    const auto familyLength = static_cast<uint16_t>(fontFace.family.size());
    const auto pathLength = static_cast<uint16_t>(fontFace.path.size());
    writeBytes(&familyLength, sizeof(familyLength));
    writeBytes(fontFace.family.data(), familyLength);
    writeBytes(&pathLength, sizeof(pathLength));
    writeBytes(fontFace.path.data(), pathLength);
    writeByte(static_cast<uint8_t>(fontFace.category));
  }

  if (!writeOk || !file.close()) {
    LOG_ERR("CSS", "Failed to write temporary CSS cache");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  const bool hadExistingCache = Storage.exists(finalPath.c_str());
  if (hadExistingCache) {
    Storage.remove(backupPath.c_str());
    if (!Storage.rename(finalPath.c_str(), backupPath.c_str())) {
      LOG_ERR("CSS", "Failed to back up existing CSS cache");
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }

  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("CSS", "Failed to promote temporary CSS cache");
    Storage.remove(tmpPath.c_str());
    if (Storage.exists(backupPath.c_str()) && !Storage.rename(backupPath.c_str(), finalPath.c_str())) {
      LOG_ERR("CSS", "Failed to restore previous CSS cache");
    }
    return false;
  }

  Storage.remove(backupPath.c_str());

  LOG_DBG("CSS", "Saved %u rules to %s cache", ruleCount, complete ? "complete" : "partial");
  return true;
}

CssParser::CacheLoadResult CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return CacheLoadResult::Invalid;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    if (!restoreCacheBackupIfNeeded() || !Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
      return CacheLoadResult::Invalid;
    }
  }

  // Clear existing rules
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return CacheLoadResult::Invalid;
  }

  uint8_t flags = 0;
  if (file.read(&flags, sizeof(flags)) != sizeof(flags) || (flags & ~CSS_CACHE_KNOWN_FLAGS) != 0) {
    LOG_DBG("CSS", "Invalid CSS cache flags: %u", flags);
    return CacheLoadResult::Invalid;
  }

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return CacheLoadResult::Invalid;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    clear();
    return CacheLoadResult::Invalid;
  }

  const PoolResult entryCapacityResult = ensureEntryCapacity(ruleCount);
  if (entryCapacityResult == PoolResult::OutOfMemory) {
    clear();
    return CacheLoadResult::LowMemory;
  }
  if (entryCapacityResult == PoolResult::Limit) {
    clear();
    return CacheLoadResult::Invalid;
  }

  auto selectorBuffer = ruleCount > 0 ? makeUniqueNoThrow<char[]>(MAX_SELECTOR_LENGTH) : nullptr;
  if (ruleCount > 0 && !selectorBuffer) {
    clear();
    return CacheLoadResult::LowMemory;
  }

  // Read each rule
  for (uint16_t i = 0; i < ruleCount; ++i) {
    // Read selector string
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      clear();
      return CacheLoadResult::Invalid;
    }

    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      clear();
      return CacheLoadResult::Invalid;
    }

    if (file.read(selectorBuffer.get(), selectorLen) != selectorLen) {
      clear();
      return CacheLoadResult::Invalid;
    }

    uint8_t styleWire[STYLE_WIRE_BYTES];
    if (file.read(styleWire, sizeof(styleWire)) != sizeof(styleWire)) {
      clear();
      return CacheLoadResult::Invalid;
    }

    CssStyle style;
    if (!decodeStyleWire(styleWire, style)) {
      clear();
      return CacheLoadResult::Invalid;
    }

    const RuleInsertResult insertResult = insertOrMerge(std::string_view(selectorBuffer.get(), selectorLen), style);
    if (insertResult == RuleInsertResult::OutOfMemory) {
      clear();
      return CacheLoadResult::LowMemory;
    }
    if (insertResult == RuleInsertResult::Limit) {
      clear();
      return CacheLoadResult::Invalid;
    }
    if (insertResult == RuleInsertResult::Merged) {
      LOG_DBG("CSS", "Duplicate selector in CSS cache");
      clear();
      return CacheLoadResult::Invalid;
    }
  }

  // Pools after the rules. Interning in order reproduces the saved 1-based indices, which
  // the rules above refer to; the saved pools hold no duplicates.
  char text[1024];
  const auto readText = [&file, &text](std::string_view& out) {
    uint16_t length = 0;
    if (file.read(&length, sizeof(length)) != sizeof(length) || length > sizeof(text)) return false;
    if (length > 0 && file.read(text, length) != length) return false;
    out = std::string_view(text, length);
    return true;
  };
  uint16_t familyCount = 0;
  if (file.read(&familyCount, sizeof(familyCount)) != sizeof(familyCount) || familyCount > MAX_FONT_FAMILIES) {
    clear();
    return CacheLoadResult::Invalid;
  }
  for (uint16_t i = 0; i < familyCount; ++i) {
    std::string_view family;
    if (!readText(family) || internFamily(family) != i + 1) {
      clear();
      return CacheLoadResult::Invalid;
    }
  }
  uint8_t faceCount = 0;
  if (file.read(&faceCount, sizeof(faceCount)) != sizeof(faceCount) || faceCount > MAX_FONT_FACES) {
    clear();
    return CacheLoadResult::Invalid;
  }
  std::string familyCopy;
  for (uint8_t i = 0; i < faceCount; ++i) {
    std::string_view family;
    std::string_view path;
    uint8_t category = 0;
    if (!readText(family)) {
      clear();
      return CacheLoadResult::Invalid;
    }
    familyCopy.assign(family.data(), family.size());  // text[] is reused for the path
    if (!readText(path) || file.read(&category, 1) != 1 || category > static_cast<uint8_t>(FontCategory::Round)) {
      clear();
      return CacheLoadResult::Invalid;
    }
    addFontFace(familyCopy, path, static_cast<FontCategory>(category));
  }

  if (file.available() != 0) {
    clear();
    return CacheLoadResult::Invalid;
  }

  const bool partial = (flags & CSS_CACHE_FLAG_PARTIAL) != 0;
  LOG_DBG("CSS", "Loaded %u rules from %s cache", ruleCount, partial ? "partial" : "complete");
  return CacheLoadResult::Complete;
}
