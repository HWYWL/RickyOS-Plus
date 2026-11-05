#pragma once

#include <cstddef>
#include <cstdint>

// Reading modes: what surrounds the text on a reader page. Pure geometry so the
// layout, the drawing and the host tests share one source; nothing here draws.
//
//   Original   the page as it always was: header, text, footer.
//   Puppy      小狗伴读: a quiet rounded frame with the logo's dog peeking over its top edge.
//   Immersive  沉浸: text only; header and footer are gone and their space goes to the text.
//   Ancient    古籍: a woodblock page set vertically (columns right to left, 界行 between
//              them), 四周双边 rules and a 版心 column on the left carrying the title and
//              the page number in Chinese numerals.
namespace readingMode {

enum class Mode : uint8_t { Original = 0, Puppy, Immersive, Ancient, Count };

constexpr Mode modeFor(const uint8_t raw) {
  return raw < static_cast<uint8_t>(Mode::Count) ? static_cast<Mode>(raw) : Mode::Original;
}

// The reader's header (time, book · chapter, battery) belongs to Original alone: the
// puppy peeks where it would sit. The footer (pages, progress) stays under the
// puppy frame too; Immersive drops both and Ancient carries its own page number.
constexpr bool showsHeader(const Mode mode) { return mode == Mode::Original; }
constexpr bool showsFooter(const Mode mode) { return mode == Mode::Original || mode == Mode::Puppy; }

// Ancient sets its text in columns (lib/Epub/Epub/VerticalText.h).
constexpr bool setsVertically(const Mode mode) { return mode == Mode::Ancient; }

// Distances from each screen edge, in logical (oriented) pixels.
struct Margins {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  constexpr bool operator==(const Margins&) const = default;
};

struct Box {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  constexpr int right() const { return x + width; }
  constexpr int bottom() const { return y + height; }
  constexpr bool operator==(const Box&) const = default;
};

// Values from the 1.2 design tokens (RickyDesignTokens.h): hairline 2, emphasis 4,
// container radius 24, spacing k2 (8) and k4 (16).
inline constexpr int kHairline = 2;
inline constexpr int kEmphasis = 4;
// A frame never sits closer than this to the panel edge. 6, not 8: at the default
// 25 px margin the puppy frame then fits without narrowing the text column.
inline constexpr int kScreenGap = 6;
inline constexpr int kTextPad = 16;  // frame line to text

// Puppy.
inline constexpr int kPuppyRadius = 24;
inline constexpr int kPuppyDogInset = 40;  // dog's right edge from the frame's right edge
inline constexpr int kPuppyHeadroom = 6;   // above the dog's ears
inline constexpr int kPuppyStatusGap = 8;  // frame bottom to the status bar text

// Ancient: 文武边栏, a thick outer rule and a thin inner one.
inline constexpr int kAncientRules = kEmphasis + 4 + kHairline;
inline constexpr int kAncientTop = 40;     // 天头 outside the frame, about twice the 地脚
inline constexpr int kAncientBottom = 16;  // 地脚
inline constexpr int kBanxinWidth = 44;    // 版心 column between the frame and the text

// Margins for the text column. `base` is the original reader layout (viewable
// inset + screen margin, plus the header and footer bands the mode keeps);
// `viewable` is the panel's own inset. `dogEdgeRow` is how far the peeking dog
// rises above the frame line. Every mode keeps at least the original margins,
// so a reader's wide-margin choice still holds.
constexpr Margins textMargins(const Mode mode, const Margins& base, const Margins& viewable, const int dogEdgeRow) {
  Margins m = base;
  const auto atLeast = [](int& value, const int floor) {
    if (value < floor) value = floor;
  };
  switch (mode) {
    case Mode::Puppy: {
      const int side = kScreenGap + kHairline + kTextPad;
      atLeast(m.left, viewable.left + side);
      atLeast(m.right, viewable.right + side);
      atLeast(m.top, viewable.top + kPuppyHeadroom + dogEdgeRow + kHairline + kTextPad);
      m.bottom = base.bottom + kTextPad + kHairline + kPuppyStatusGap;
      break;
    }
    case Mode::Ancient: {
      // Columns sit flush against the rules: the frame's inner rule is the first 界行.
      atLeast(m.right, viewable.right + kScreenGap + kAncientRules);
      atLeast(m.left, viewable.left + kScreenGap + kAncientRules + kBanxinWidth + kHairline);
      atLeast(m.top, viewable.top + kAncientTop + kAncientRules + kTextPad);
      atLeast(m.bottom, viewable.bottom + kAncientBottom + kAncientRules + kTextPad);
      break;
    }
    case Mode::Original:
    case Mode::Immersive:
    case Mode::Count:
      break;
  }
  return m;
}

constexpr Box textBox(const int screenWidth, const int screenHeight, const Margins& m) {
  return Box{m.left, m.top, screenWidth - m.left - m.right, screenHeight - m.top - m.bottom};
}

// Puppy: the frame line's outer edge, kTextPad outside the text.
constexpr Box puppyFrame(const Box& text) {
  const int out = kTextPad + kHairline;
  return Box{text.x - out, text.y - out, text.width + 2 * out, text.height + 2 * out};
}

// Ancient: the outer rule's outer edge. Columns run right to left, so the 版心 (the
// fold of the leaf) closes the page on the left, after the last column.
constexpr Box ancientFrame(const Box& text) {
  const int out = kTextPad + kAncientRules;
  const int left = kAncientRules + kBanxinWidth + kHairline;
  return Box{text.x - left, text.y - out, text.width + left + kAncientRules, text.height + 2 * out};
}

// The 版心 column: between the frame's inner rule and the rule beside the text.
constexpr Box banxin(const Box& text) {
  const Box frame = ancientFrame(text);
  return Box{frame.x + kAncientRules, frame.y + kAncientRules, kBanxinWidth, frame.height - 2 * kAncientRules};
}

// Ancient: trims the text box to whole columns of `pitch` (the line height) so every
// 界行 stays on the grid, then centres the frame on the screen with the trimmed width.
constexpr Margins fitColumns(Margins m, const Margins& viewable, const int screenWidth, const int pitch) {
  constexpr int kLeftChrome = kAncientRules + kBanxinWidth + kHairline;
  const int width = screenWidth - m.left - m.right;
  if (pitch <= 0 || width <= pitch) return m;
  // Paper outside the frame on both sides, once the partial column is given up.
  const int gaps = (m.left - kLeftChrome) + (m.right - kAncientRules) + width % pitch;
  const int rightFloor = viewable.right + kScreenGap;
  const int leftFloor = viewable.left + kScreenGap;
  int right = gaps / 2;
  if (right > gaps - leftFloor) right = gaps - leftFloor;
  if (right < rightFloor) right = rightFloor;
  m.right = right + kAncientRules;
  m.left = gaps - right + kLeftChrome;
  return m;
}

// Chinese numerals for a page number: 1 一, 10 十, 12 十二, 20 二十, 101 一百零一, 1010 一千零一十.
// Writes UTF-8 into `out` and returns its byte length; numbers past 9999, or a
// buffer too small, fall back to ASCII digits.
inline size_t chineseNumeral(const int value, char* out, const size_t size) {
  if (size == 0) return 0;
  out[0] = '\0';
  static constexpr const char* kDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  static constexpr const char* kUnits[] = {"", "十", "百", "千"};
  size_t length = 0;
  const auto append = [&](const char* text) {
    for (const char* p = text; *p; ++p) {
      if (length + 1 >= size) return false;
      out[length++] = *p;
    }
    out[length] = '\0';
    return true;
  };
  const auto ascii = [&]() {
    length = 0;
    out[0] = '\0';
    char digits[12];
    int v = value < 0 ? 0 : value, n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v > 0 && n < 11);
    while (n > 0 && length + 1 < size) out[length++] = digits[--n];
    out[length] = '\0';
    return length;
  };
  if (value < 0 || value > 9999) return ascii();
  if (value == 0) return append(kDigits[0]) ? length : ascii();
  int digits[4] = {value / 1000, value / 100 % 10, value / 10 % 10, value % 10};
  int first = 0;
  while (digits[first] == 0) ++first;
  bool pendingZero = false;
  for (int i = first; i < 4; ++i) {
    const int unit = 3 - i;
    if (digits[i] == 0) {
      pendingZero = true;
      continue;
    }
    if (pendingZero && !append(kDigits[0])) return ascii();
    pendingZero = false;
    // 十二, not 一十二, when ten leads; 一百一十 keeps it.
    const bool bareTen = unit == 1 && digits[i] == 1 && i == first;
    if (!bareTen && !append(kDigits[digits[i]])) return ascii();
    if (!append(kUnits[unit])) return ascii();
  }
  return length;
}

}  // namespace readingMode
