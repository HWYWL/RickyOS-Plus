#pragma once
#include <FreeInkUIIcon.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "RickyDesignTokens.h"
#include "RickyPageLayout.h"
#include "UiAppHost.h"
#include "components/RickyAaIcons.h"

namespace RickyPageUi {
namespace fui = freeink::ui;

inline fui::Rect uiRect(const Rect& r) {
  return {static_cast<int16_t>(r.x), static_cast<int16_t>(r.y), static_cast<int16_t>(r.width),
          static_cast<int16_t>(r.height)};
}
// Native wrappedText truncates a CJK run with no spaces as a single word.
// Use the SDK's bounded UTF-8 character wrapping, then emit single-line runs.
// This also makes drawing agree with measureWrappedText, without a wrap vector.
inline void wrappedText(fui::DrawTarget& target, fui::Rect rect, const char* text, fui::TextStyle style) {
  const int lineHeight = target.lineHeight(style.font);
  if (rect.empty() || lineHeight <= 0 || rect.height < lineHeight) return;
  style.maxLines = std::clamp<int>(style.maxLines, 1, rect.height / lineHeight);
  fui::layoutText(target, rect, text, style, [&](const char* line, fui::Rect bounds) {
    bounds.width = std::min<int>(bounds.width, rect.right() - bounds.x);
    auto run = style;
    run.maxLines = 1;
    run.align = fui::TextAlign::Left;  // layoutText has already positioned this run.
    target.text(bounds, line, run);
  });
}
inline void arrow(fui::DrawTarget& target, fui::Rect r) {
  const auto black = fui::Paint::solid(fui::Color::Black);
  const int16_t mid = r.y + r.height / 2;
  const int16_t end = r.right() - 2;
  target.line({r.x, mid}, {end, mid}, 2, black);
  target.line({static_cast<int16_t>(end - 7), static_cast<int16_t>(mid - 7)}, {end, mid}, 2, black);
  target.line({static_cast<int16_t>(end - 7), static_cast<int16_t>(mid + 7)}, {end, mid}, 2, black);
}

// The same inversion recipe on every main-page card.
constexpr uint8_t CARD_RADIUS = ricky::ds::radius::kContainer;
inline void card(fui::DrawTarget& target, fui::Rect rect, bool focused) {
  target.fill(rect, fui::Paint::solid(focused ? fui::Color::Black : fui::Color::White), CARD_RADIUS);
  // At rest a light-gray edge (exact on a 16-gray frame, ink on a B/W one).
  if (!focused) target.stroke(rect, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kHairline, CARD_RADIUS);
}

// Pre-rendered Lucide icon (rickyPageIcons.h), optically centred on `centerY`.
inline void pageIcon(fui::DrawTarget& target, const GfxRenderer& renderer, int x, int centerY,
                     const freeink::Icon& icon, bool inverted = false) {
  // Settled screens draw the anti-aliased twin; the B/W pass keeps the 1-bpp icon.
  if (RickyAaIcons::draw(renderer, icon.bits, icon.w, icon.h, x, centerY - icon.opticalCenterY, !inverted)) return;
  target.bitmap(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(centerY - icon.opticalCenterY),
                          static_cast<int16_t>(icon.w), static_cast<int16_t>(icon.h)},
                fui::bitmapFromIcon(icon), fui::BitmapMode::Center,
                fui::Paint::solid(inverted ? fui::Color::White : fui::Color::Black));
}

// Usage ring: hairline track, `percent` of it filled clockwise from 12 o'clock.
inline void usageRing(const GfxRenderer& renderer, Rect box, int percent, int thickness, bool inverted = false) {
  const int size = std::min(box.width, box.height);
  if (size <= 0) return;
  const float outer = size / 2.0f, inner = outer - static_cast<float>(thickness);
  const float cx = box.x + outer - 0.5f, cy = box.y + outer - 0.5f;
  const float limit = 6.2831853f * static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = box.x + x - cx, dy = box.y + y - cy;
      const float r = std::sqrt(dx * dx + dy * dy);
      if (r > outer || r < inner) continue;
      float angle = std::atan2(dx, -dy);
      if (angle < 0) angle += 6.2831853f;
      const bool edge = r > outer - 1.5f || r < inner + 1.5f;
      if (angle <= limit || edge) renderer.drawPixel(box.x + x, box.y + y, !inverted);
    }
  }
}

// Quiet secondary affordance for rows that open a list or page.
inline void chevron(fui::DrawTarget& target, fui::Rect r, bool inverted = false) {
  const auto black = fui::Paint::solid(inverted ? fui::Color::White : fui::Color::Black);
  const int16_t mid = r.y + r.height / 2;
  const int16_t tip = r.right() - 3;
  target.line({static_cast<int16_t>(tip - 7), static_cast<int16_t>(mid - 8)}, {tip, mid}, 2, black);
  target.line({tip, mid}, {static_cast<int16_t>(tip - 7), static_cast<int16_t>(mid + 8)}, 2, black);
}

// Compatibility for older functional pages while they move to the new type
// roles. UI weights now come from actual font faces, never pixel dilation.
struct Bold {
  explicit Bold(const GfxRenderer&, uint8_t) {}
};

inline void progressBar(fui::DrawTarget& target, fui::Rect r, int percent) {
  target.fill(r, fui::Paint::dither(fui::Color::LightGray));
  const int filled = r.width * std::clamp(percent, 0, 100) / 100;
  if (filled > 0)
    target.fill(fui::Rect{r.x, r.y, static_cast<int16_t>(filled), r.height}, fui::Paint::solid(fui::Color::Black));
}

// Generated covers pick one small line motif from the title so a shelf of
// cover-less books stays distinguishable. Deterministic, no assets or heap.
enum class Motif : uint8_t { Tree, Star, Flower, Leaf, Mountain, Wave, Count };
inline Motif motifFor(const char* text) {
  uint32_t hash = 2166136261u;
  for (const char* p = text ? text : ""; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
  return static_cast<Motif>(hash % static_cast<uint32_t>(Motif::Count));
}
inline void motif(fui::DrawTarget& target, Rect r, Motif kind) {
  const auto black = fui::Paint::solid(fui::Color::Black);
  const int x = r.x, y = r.y, s = r.width;
  const auto at = [&](int px, int py) {
    return fui::Point{static_cast<int16_t>(x + px * s / 24), static_cast<int16_t>(y + py * s / 24)};
  };
  const auto path = [&](const int (*points)[2], int count, bool closed) {
    for (int i = 0; i + 1 < count; ++i)
      target.line(at(points[i][0], points[i][1]), at(points[i + 1][0], points[i + 1][1]), 2, black);
    if (closed) target.line(at(points[count - 1][0], points[count - 1][1]), at(points[0][0], points[0][1]), 2, black);
  };
  const auto circle = [&](int cx, int cy, int radius) {
    const auto rect =
        uiRect(Rect{x + (cx - radius) * s / 24, y + (cy - radius) * s / 24, radius * 2 * s / 24, radius * 2 * s / 24});
    target.stroke(rect, black, 2, static_cast<uint8_t>(rect.width / 2));
  };
  switch (kind) {
    case Motif::Tree: {
      circle(12, 7, 5);
      circle(7, 12, 4);
      circle(17, 12, 4);
      static constexpr int trunk[][2] = {{12, 12}, {12, 22}};
      static constexpr int ground[][2] = {{7, 22}, {17, 22}};
      path(trunk, 2, false);
      path(ground, 2, false);
      break;
    }
    case Motif::Star: {
      static constexpr int star[][2] = {{12, 2},  {14, 9}, {22, 9}, {16, 14}, {18, 21},
                                        {12, 17}, {6, 21}, {8, 14}, {2, 9},   {10, 9}};
      path(star, 10, true);
      break;
    }
    case Motif::Flower: {
      circle(12, 5, 3);
      circle(17, 10, 3);
      circle(7, 10, 3);
      circle(12, 15, 3);
      static constexpr int stem[][2] = {{12, 18}, {12, 23}};
      static constexpr int leaf[][2] = {{12, 21}, {16, 19}};
      path(stem, 2, false);
      path(leaf, 2, false);
      break;
    }
    case Motif::Leaf: {
      static constexpr int outline[][2] = {{4, 20},  {4, 13},  {7, 8},   {12, 5}, {20, 4},
                                           {19, 12}, {16, 17}, {11, 20}, {4, 20}};
      static constexpr int rib[][2] = {{4, 20}, {15, 9}};
      static constexpr int stem[][2] = {{2, 22}, {4, 20}};
      path(outline, 9, false);
      path(rib, 2, false);
      path(stem, 2, false);
      break;
    }
    case Motif::Mountain: {
      static constexpr int ridge[][2] = {{2, 21}, {9, 9}, {13, 15}, {16, 11}, {22, 21}};
      static constexpr int base[][2] = {{2, 21}, {22, 21}};
      path(ridge, 5, false);
      path(base, 2, false);
      circle(18, 5, 2);
      break;
    }
    case Motif::Wave:
    case Motif::Count: {
      static constexpr int upper[][2] = {{2, 10}, {5, 7}, {8, 10}, {11, 13}, {14, 10}, {17, 7}, {20, 10}, {22, 12}};
      static constexpr int lower[][2] = {{2, 16}, {5, 13}, {8, 16}, {11, 19}, {14, 16}, {17, 13}, {20, 16}, {22, 18}};
      path(upper, 8, false);
      path(lower, 8, false);
      break;
    }
  }
}

// Index titles fall back to the file name; show "浮生六记", not "浮生六记.txt".
inline void stripBookExtension(std::string& title) {
  static constexpr const char* extensions[] = {".epub", ".txt", ".md"
#if defined(RICKYOS_KINDLE_IMPORT)
    , ".mobi", ".azw3"
#endif
  };
  for (const char* extension : extensions) {
    const size_t length = strlen(extension);
    if (title.size() <= length) continue;
    bool match = true;
    for (size_t i = 0; i < length && match; ++i)
      match = tolower(static_cast<unsigned char>(title[title.size() - length + i])) == extension[i];
    if (match) {
      title.resize(title.size() - length);
      return;
    }
  }
}

// Covers carry the family name, like a spine: the part after the last
// middle dot ("亨利·戴维·梭罗" -> "梭罗"). Returns the whole name otherwise.
inline const char* coverAuthor(const char* author) {
  if (!author) return nullptr;
  const char* tail = author;
  for (const char* p = author; *p; ++p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    if (u[0] == 0xC2 && u[1] == 0xB7)
      tail = p + 2;  // U+00B7
    else if (u[0] == 0xE3 && u[1] == 0x83 && u[2] == 0xBB)
      tail = p + 3;  // U+30FB
    else if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0xA2 || u[2] == 0xA7))
      tail = p + 3;  // U+2022/2027
  }
  return *tail ? tail : author;
}

// Typographic cover for books without artwork: double rule, author, title,
// motif and an optional footer (format). `text` is a full-coverage face; the
// title is drawn bold, so callers keep a Bold guard alive.
inline void generatedCover(fui::DrawTarget& target, fui::Rect rect, const char* title, const char* author,
                           const char* footer, fui::TextStyle text) {
  if (rect.empty()) return;
  const auto black = fui::Paint::solid(fui::Color::Black);
  target.fill(rect, fui::Paint::solid(fui::Color::White));
  target.stroke(rect, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kCoverEdge);
  const int16_t rule = std::max<int16_t>(5, rect.width / 22);
  if (rect.width > rule * 4 && rect.height > rule * 4)
    target.stroke(fui::Rect{static_cast<int16_t>(rect.x + rule), static_cast<int16_t>(rect.y + rule),
                            static_cast<int16_t>(rect.width - rule * 2), static_cast<int16_t>(rect.height - rule * 2)},
                  black, 1);
  const int lineHeight = target.lineHeight(text.font);
  const int pad = rule + std::max(6, rect.width / 14);
  const int inner = rect.width - pad * 2;
  if (lineHeight <= 0 || inner <= 0) return;
  text.align = fui::TextAlign::Center;
  text.maxLines = 1;
  const bool roomy = rect.height >= lineHeight * 6;
  int top = rect.y + pad, bottom = rect.bottom() - pad;
  author = coverAuthor(author);
  if (roomy && author && *author && target.measureText(text.font, author, text).width <= inner) {
    target.text(fui::Rect{static_cast<int16_t>(rect.x + pad), static_cast<int16_t>(top), static_cast<int16_t>(inner),
                          static_cast<int16_t>(lineHeight)},
                author, text);
    top += lineHeight;
  }
  if (roomy && footer && *footer) {
    bottom -= lineHeight;
    target.text(fui::Rect{static_cast<int16_t>(rect.x + pad), static_cast<int16_t>(bottom), static_cast<int16_t>(inner),
                          static_cast<int16_t>(lineHeight)},
                footer, text);
  }
  auto heading = text;
  heading.bold = true;
  heading.maxLines = std::clamp((bottom - top) / lineHeight - 1, 1, 3);
  const int titleHeight = fui::measureWrappedText(target, title, heading, inner).height;
  const int size = std::clamp(rect.width / 4, 18, 40);
  const int gap = std::max(6, lineHeight / 3);
  const bool drawMotif = bottom - top >= titleHeight + gap + size;
  const int group = titleHeight + (drawMotif ? gap + size : 0);
  const int y = top + std::max(0, (bottom - top - group) / 2);
  wrappedText(target,
              fui::Rect{static_cast<int16_t>(rect.x + pad), static_cast<int16_t>(y), static_cast<int16_t>(inner),
                        static_cast<int16_t>(titleHeight)},
              title, heading);
  if (drawMotif)
    motif(target, Rect{rect.x + (rect.width - size) / 2, y + titleHeight + gap, size, size}, motifFor(title));
}

// Landscape "recently opened" tile: motif + one-line title inside a hairline frame.
inline void bookTile(fui::DrawTarget& target, fui::Rect rect, const char* title, fui::TextStyle text,
                     Motif kind = Motif::Count) {
  if (rect.empty()) return;
  target.fill(rect, fui::Paint::solid(fui::Color::White));
  target.stroke(rect, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kCoverEdge);
  const int lineHeight = target.lineHeight(text.font);
  const int size = std::min<int>(lineHeight, rect.height / 2);
  const int gap = std::max(8, lineHeight / 3);
  const int maxText = std::max(0, rect.width - size - gap - gap * 4);
  text.maxLines = 1;
  text.align = fui::TextAlign::Left;
  const int textWidth = std::min<int>(maxText, target.measureText(text.font, title, text).width);
  const int x = rect.x + std::max(gap * 2, (rect.width - size - gap - textWidth) / 2);
  motif(target, Rect{x, rect.y + (rect.height - size) / 2, size, size}, kind == Motif::Count ? motifFor(title) : kind);
  target.text(
      fui::Rect{static_cast<int16_t>(x + size + gap), static_cast<int16_t>(rect.y + (rect.height - lineHeight) / 2),
                static_cast<int16_t>(maxText), static_cast<int16_t>(lineHeight)},
      title, text);
}

inline int syncNav(fui::ListNav& nav, int count) {
  fui::ListProps paging;
  nav.syncToProps(fui::Rect{0, 0, 1, static_cast<int16_t>(count)}, 1, 0, count, paging, 0);
  nav.drawnCount = count;
  nav.onListRendered(0, count, paging.selectedIndex >= 0 && paging.selectedIndex < count);
  return paging.selectedIndex;
}

}  // namespace RickyPageUi
