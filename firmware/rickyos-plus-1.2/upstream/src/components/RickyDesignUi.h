#pragma once

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstring>

#include "RickyDesignTokens.h"
#include "RickyGrayStroke.h"
#include "RickyPageLayout.h"
#include "UiAppHost.h"

// Product-only immutable font IDs, independent of generated reader font IDs.
namespace RickyDesignUi {
namespace ds = ricky::ds;
namespace fui = freeink::ui;
inline constexpr int BODY = 0x524401;
inline constexpr int CAPTION = 0x524402;
inline constexpr int HEADLINE = 0x524403;
inline constexpr int TITLE = 0x524404;
inline constexpr int NUMERIC_DISPLAY = 0x524405;
// Real Medium at BODY's 12pt size/36px line, with the same codepoint coverage.
// Keep general body, author/progress and the user's reader family independent.
inline constexpr int BOOK_TITLE = 0x524406;

// Whole-run selection: never silently mix bold/regular glyphs in a title.
inline int fontFor(const GfxRenderer& renderer, int preferred, const char* text) {
  if (!text || preferred == BODY) return preferred;
  const auto it = renderer.getFontMap().find(preferred);
  if (it == renderer.getFontMap().end()) return BODY;
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text);
  for (uint32_t cp; (cp = utf8NextCodepoint(&cursor)) != 0;)
    if (!it->second.hasCodepoint(cp)) return BODY;
  return preferred;
}

inline int bookTitleFontFor(const GfxRenderer& renderer, const char* title, int preferred = BOOK_TITLE) {
  const int resolved = fontFor(renderer, preferred, title);
  return resolved != BODY ? resolved : fontFor(renderer, BOOK_TITLE, title);
}

// Exact native tones; B/W path uses the spec fallback, never Bayer dots.
inline void fill(const GfxRenderer& renderer, fui::Rect r, ds::gray::Level tone, int radius = 0) {
  if (r.empty()) return;
  if (!renderer.isGrayscale16Active() || tone == ds::gray::kInk || tone == ds::gray::kPaper) {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, radius,
                             ds::gray::oneBitInk(tone) ? Color::Black : Color::White);
    return;
  }
  radius = std::max(0, std::min({radius, r.width / 2, r.height / 2}));
  // Straight parts go row by row in panel memory; only the four corner squares are
  // per-pixel. No frame/scratch allocation.
  renderer.fillGrayscale16Rect(r.x, r.y + radius, r.width, r.height - 2 * radius, tone);
  renderer.fillGrayscale16Rect(r.x + radius, r.y, r.width - 2 * radius, radius, tone);
  renderer.fillGrayscale16Rect(r.x + radius, r.y + r.height - radius, r.width - 2 * radius, radius, tone);
  if (radius == 0) return;
  for (int x = 0; x < r.width; ++x) {
    if (x == radius) x = r.width - radius;  // skip the middle columns, already filled
    for (int y = 0; y < r.height; ++y) {
      if (y == radius) y = r.height - radius;
      const int dx = x < radius ? radius - x : x >= r.width - radius ? x - (r.width - radius - 1) : 0;
      const int dy = y < radius ? radius - y : y >= r.height - radius ? y - (r.height - radius - 1) : 0;
      if (dx && dy && dx * dx + dy * dy > radius * radius) continue;
      renderer.drawGrayscale16Pixel(r.x + x, r.y + y, ds::gray::toRgb8(tone));
    }
  }
}

inline void card(const GfxRenderer& renderer, fui::Rect r, bool selected) {
  fill(renderer, r, selected ? ds::gray::kInk : ds::gray::kPaper, ds::radius::kContainer);
  if (!selected)
    renderer.drawRoundedRect(r.x, r.y, r.width, r.height, ds::stroke::kHairline, ds::radius::kContainer, true);
}

// A resting card: paper with a light-gray hairline edge (exact kLine on a 16-gray frame,
// the 1-bit fallback ink on a fast B/W frame). Selected stays the inverted ink card.
inline void softCard(const GfxRenderer& renderer, fui::Rect r, bool selected) {
  if (selected || !renderer.isGrayscale16Active()) {
    card(renderer, r, selected);
    return;
  }
  constexpr int edge = ds::stroke::kHairline;
  fill(renderer, r, ds::gray::kLine, ds::radius::kContainer);
  fill(renderer,
       {static_cast<int16_t>(r.x + edge), static_cast<int16_t>(r.y + edge), static_cast<int16_t>(r.width - 2 * edge),
        static_cast<int16_t>(r.height - 2 * edge)},
       ds::gray::kPaper, ds::radius::kContainer - edge);
}

// A section rule: one light-gray hairline instead of a box around the section.
inline void rule(const GfxRenderer& renderer, fui::Rect r) {
  fill(renderer, {r.x, r.y, r.width, ds::stroke::kHairline}, ds::gray::kLine);
}

// `lineTrack` draws the unread part as a 1 px dotted line, pure black and white: the
// Plus reader re-pulses unchanged gray toward black on each page turn, so a gray track
// there darkens until read and unread look the same, and a solid hairline is as thick
// as the bar itself.
inline void progress(const GfxRenderer& renderer, fui::Rect r, int percent, bool inverted = false,
                     bool lineTrack = false) {
  const auto ink = inverted ? ds::gray::kPaper : ds::gray::kInk;
  if (lineTrack && !inverted) {
    const auto y = static_cast<int16_t>(r.y + r.height / 2);
    for (int x = r.x; x < r.x + r.width; x += 4) fill(renderer, {static_cast<int16_t>(x), y, 1, 1}, ink);
  } else if (renderer.isGrayscale16Active() && !inverted)
    fill(renderer, r, ds::gray::kTint, r.height / 2);
  else
    fill(renderer,
         {r.x, static_cast<int16_t>(r.y + (r.height - ds::stroke::kHairline) / 2), r.width, ds::stroke::kHairline},
         ink);
  const int width = r.width * std::clamp(percent, 0, 100) / 100;
  if (width) fill(renderer, {r.x, r.y, static_cast<int16_t>(width), r.height}, ink, r.height / 2);
}

inline void text(const GfxRenderer& renderer, fui::Rect r, const char* value, int font = BODY, bool inverted = false,
                 fui::TextAlign align = fui::TextAlign::Left, int lines = 1, ds::gray::Level tone = ds::gray::kInk) {
  // A tiny stack adapter avoids changing the host's published font bindings.
  // Layout's fixed-capacity UTF-8 wrapping avoids per-line heap vectors.
  RickyUiTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_BODY, fontFor(renderer, font, value));
  fui::TextStyle style;
  style.font = fui::GfxRendererTarget::FONT_BODY;
  style.color = inverted                  ? fui::Color::White
                : tone == ds::gray::kInk2 ? fui::Color::DarkGray
                : tone == ds::gray::kMute ? fui::Color::LightGray
                                          : fui::Color::Black;
  style.align = align;
  style.maxLines = lines;
  const int height = target.lineHeight(style.font);
  if (height <= 0 || r.empty()) return;
  if (r.height < height) {
    // A box shorter than the face (an SD reading font can be taller than the built-in
    // one the layout was measured with) gets one whole line centred on it, never
    // glyphs cut in half.
    const fui::Rect line{r.x, static_cast<int16_t>(r.y - (height - r.height) / 2), r.width,
                         static_cast<int16_t>(height)};
    const GfxRenderer::ClipScope clip(renderer, line.x, line.y, line.width, line.height);
    style.maxLines = 1;
    target.text(line, value, style);
    return;
  }
  const GfxRenderer::ClipScope clip(renderer, r.x, r.y, r.width, r.height);
  style.maxLines = std::min<int>(style.maxLines, r.height / height);
  fui::layoutText(target, r, value, style, [&](const char* run, fui::Rect bounds) {
    auto single = style;
    single.maxLines = 1;
    single.align = fui::TextAlign::Left;
    target.text(bounds, run, single);
  });
}

inline void bookTitle(const GfxRenderer& renderer, fui::Rect r, const char* value, int preferred = BOOK_TITLE,
                      bool inverted = false, fui::TextAlign align = fui::TextAlign::Left, int lines = 1) {
  text(renderer, r, value, bookTitleFontFor(renderer, value, preferred), inverted, align, lines);
}

inline void secondaryText(const GfxRenderer& renderer, fui::Rect r, const char* value, int font = CAPTION,
                          bool inverted = false, fui::TextAlign align = fui::TextAlign::Left, int lines = 1) {
  text(renderer, r, value, font, inverted, align, lines, ds::gray::kInk2);
}

inline void cover(const GfxRenderer& renderer, fui::Rect r, const char* title, const char* format) {
  if (r.empty()) return;
  const GfxRenderer::ClipScope clip(renderer, r.x, r.y, r.width, r.height);
  const int style = ds::component::coverTemplateFor(title);
  const bool dark = style == 0 || style == 3;
  fill(renderer, r, dark ? ds::gray::kInk : style == 4 ? ds::gray::kTint : ds::gray::kPaper);
  // Light templates get the same light gray edge as cover artwork, not a black frame.
  if (dark)
    renderer.drawRect(r.x, r.y, r.width, r.height, ds::stroke::kHairline, false);
  else
    RickyGrayStroke::ring(renderer, r.x, r.y, r.width, r.height, ds::stroke::kCoverEdge, 0, ds::gray::kLine);
  const bool compact = r.width < 120 || r.height < 180;
  const int pad = compact ? ds::space::k2 : ds::space::k4;
  if (style == 2) {
    const int band = std::min<int>(ds::space::k12, r.height / 4);
    fill(renderer, {r.x, static_cast<int16_t>(r.bottom() - band), r.width, static_cast<int16_t>(band)}, ds::gray::kInk);
  } else if (style == 5) {
    renderer.drawRect(r.x + pad / 2, r.y + pad / 2, r.width - pad, r.height - pad, ds::stroke::kHairline, true);
  } else if (style == 4) {
    // Thin, widely-spaced rules rather than a checkerboard texture. Paper in
    // interaction frames; exact named line gray in resting frames.
    if (renderer.isGrayscale16Active())
      for (int y = r.bottom() - 64; y < r.bottom() - 40; y += 8)
        fill(renderer,
             {static_cast<int16_t>(r.x + 8), static_cast<int16_t>(y), static_cast<int16_t>(r.width - 16),
              ds::stroke::kHairline},
             ds::gray::kLine);
  }
  // Recent-book thumbnails share the cover templates, not full-size type.
  // Format is already shown below the thumbnail; keep that space for its title.
  const int font = fontFor(renderer, compact ? CAPTION : HEADLINE, title);
  const int lh = renderer.getLineHeight(font);
  int chars = 0;
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(title ? title : "");
  bool cjk = true;
  for (uint32_t cp; (cp = utf8NextCodepoint(&cursor)) != 0;) {
    ++chars;
    cjk &= cp >= 0x3000;
  }
  const bool vertical = r.width >= 160 && chars > 0 && chars <= 5 && cjk && chars * lh <= r.height - 64;
  const fui::Rect label{static_cast<int16_t>(r.x + pad), static_cast<int16_t>(r.y + pad),
                        static_cast<int16_t>(std::max(1, r.width - pad * 2)),
                        static_cast<int16_t>(std::max(1, r.height - pad * 2 - (compact ? 0 : 32)))};
  if (vertical) {
    cursor = reinterpret_cast<const uint8_t*>(title);
    int y = label.y + std::max(0, (label.height - chars * lh) / 2);
    while (*cursor) {
      const uint8_t* begin = cursor;
      utf8NextCodepoint(&cursor);
      char glyph[5]{};
      const size_t bytes = std::min<size_t>(4, cursor - begin);
      memcpy(glyph, begin, bytes);
      text(renderer, {label.x, static_cast<int16_t>(y), label.width, static_cast<int16_t>(lh)}, glyph, font, dark,
           fui::TextAlign::Center);
      y += lh;
    }
  } else {
    const int rows = std::min(3, label.height / std::max(1, lh));
    auto centered = label;
    centered.y += std::max(0, (label.height - rows * lh) / 2);
    centered.height = rows * lh;
    text(renderer, centered, title, font, dark, fui::TextAlign::Center, rows);
  }
  if (!compact)
    text(renderer,
         {static_cast<int16_t>(r.x + pad), static_cast<int16_t>(r.bottom() - pad - 28),
          static_cast<int16_t>(r.width - pad * 2), 28},
         format ? format : "", CAPTION, dark || style == 2);
}
}  // namespace RickyDesignUi
