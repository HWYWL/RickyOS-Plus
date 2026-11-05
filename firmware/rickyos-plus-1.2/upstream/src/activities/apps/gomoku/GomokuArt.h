#pragma once

#include <GfxRenderer.h>

#include <algorithm>

#include "components/RickyDesignUi.h"

// One RickyOS drawing for both gomoku boards (traditional and skill), so they match
// each other and the system: a paper plate with the card hairline, light-gray inner
// lines inside an ink frame, ink star points, ink / paper stones, and the system's
// rounded focus ring. Tones come from RickyDesignTokens; on a B/W frame each tone
// falls back to the spec's one-bit ink or paper.
namespace gomokuart {
namespace ds = ricky::ds;
namespace fui = freeink::ui;

inline fui::Rect rect(const int x, const int y, const int w, const int h) {
  return {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(std::max(0, w)),
          static_cast<int16_t>(std::max(0, h))};
}

// The board: plate, grid, frame and star points. (ox, oy) is the top-left
// intersection; the plate reaches half a pitch past the outer lines, which is also
// the board's touch tolerance.
inline void board(const GfxRenderer& renderer, const int ox, const int oy, const int pitch, const int n) {
  if (pitch <= 0 || n < 2) return;
  const int len = (n - 1) * pitch;
  const int pad = pitch / 2;
  RickyDesignUi::softCard(renderer, rect(ox - pad, oy - pad, len + 2 * pad, len + 2 * pad), false);
  constexpr int line = 2;
  for (int i = 1; i < n - 1; ++i) {
    RickyDesignUi::fill(renderer, rect(ox, oy + i * pitch - line / 2, len, line), ds::gray::kLine);
    RickyDesignUi::fill(renderer, rect(ox + i * pitch - line / 2, oy, line, len), ds::gray::kLine);
  }
  constexpr int frame = 3;
  RickyDesignUi::fill(renderer, rect(ox - 1, oy - 1, len + frame, frame), ds::gray::kInk);
  RickyDesignUi::fill(renderer, rect(ox - 1, oy + len - 1, len + frame, frame), ds::gray::kInk);
  RickyDesignUi::fill(renderer, rect(ox - 1, oy - 1, frame, len + frame), ds::gray::kInk);
  RickyDesignUi::fill(renderer, rect(ox + len - 1, oy - 1, frame, len + frame), ds::gray::kInk);
  // Star points: the four 3-3 points and the centre (15x15), or 2-2 and centre (9x9).
  const int edge = n >= 13 ? 3 : 2;
  const int dot = std::max(3, pitch / 9);
  const int points[5][2] = {{edge, edge}, {edge, n - 1 - edge}, {n / 2, n / 2}, {n - 1 - edge, edge}, {n - 1 - edge, n - 1 - edge}};
  for (const auto& p : points) {
    RickyDesignUi::fill(renderer, rect(ox + p[1] * pitch - dot, oy + p[0] * pitch - dot, 2 * dot + 1, 2 * dot + 1),
                        ds::gray::kInk, dot);
  }
}

// A stone centred on an intersection. White is a paper disc inside an ink ring, so it
// reads on the plate and hides the grid lines under it.
inline void stone(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const bool black) {
  if (radius <= 0) return;
  const auto disc = rect(cx - radius, cy - radius, 2 * radius + 1, 2 * radius + 1);
  RickyDesignUi::fill(renderer, disc, ds::gray::kInk, radius);
  if (black) return;
  const int ring = std::max(2, radius / 7);
  RickyDesignUi::fill(renderer, rect(cx - radius + ring, cy - radius + ring, 2 * (radius - ring) + 1, 2 * (radius - ring) + 1),
                      ds::gray::kPaper, radius - ring);
}

// The most recent move: a small dot in the opposite tone at the stone's centre.
inline void lastMove(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const bool onBlack) {
  const int dot = std::max(2, radius / 4);
  RickyDesignUi::fill(renderer, rect(cx - dot, cy - dot, 2 * dot + 1, 2 * dot + 1),
                      onBlack ? ds::gray::kPaper : ds::gray::kInk, dot);
}

// The aiming cursor: the system's rounded focus ring around one intersection.
inline void cursor(const GfxRenderer& renderer, const int cx, const int cy, const int pitch) {
  const int half = std::max(6, pitch / 2 - 2);
  renderer.drawRoundedRect(cx - half, cy - half, 2 * half, 2 * half, ds::stroke::kHairline,
                           std::min<int>(ds::radius::kControl, half / 2), true);
}

// A player card: the side's stone, its name and one detail line (count or energy).
// The side to move is the inverted (selected) card, as everywhere else in RickyOS.
inline void playerCard(const GfxRenderer& renderer, const fui::Rect r, const bool black, const char* name,
                       const char* detail, const bool active) {
  RickyDesignUi::softCard(renderer, r, active);
  const int radius = std::max(8, std::min<int>(r.height / 4, 20));
  const int cx = r.x + ds::space::kCardPadding + radius;
  const int cy = r.y + r.height / 2;
  if (active) {
    // On the ink card a black stone needs a paper rim to stay visible.
    RickyDesignUi::fill(renderer, rect(cx - radius - 2, cy - radius - 2, 2 * radius + 5, 2 * radius + 5),
                        ds::gray::kPaper, radius + 2);
  }
  stone(renderer, cx, cy, radius, black);
  const int textX = cx + radius + ds::space::kInlineGap;
  const int textW = r.x + r.width - ds::space::kCardPadding - textX;
  const int nameH = renderer.getLineHeight(RickyDesignUi::BODY);
  const int detailH = renderer.getLineHeight(RickyDesignUi::CAPTION);
  const int top = r.y + (r.height - nameH - detailH - ds::space::kTextStackGap) / 2;
  RickyDesignUi::text(renderer, rect(textX, top, textW, nameH), name, RickyDesignUi::BODY, active);
  RickyDesignUi::text(renderer, rect(textX, top + nameH + ds::space::kTextStackGap, textW, detailH), detail,
                      RickyDesignUi::CAPTION, active, fui::TextAlign::Left, 1,
                      active ? ds::gray::kPaper : ds::gray::kInk2);
}

// A menu row card: label (and optional detail), inverted when selected, muted when off.
inline void rowCard(const GfxRenderer& renderer, const fui::Rect r, const char* label, const char* detail,
                    const bool selected, const bool enabled) {
  RickyDesignUi::softCard(renderer, r, selected && enabled);
  const bool inverted = selected && enabled;
  const auto tone = enabled ? ds::gray::kInk : ds::gray::kMute;
  const int x = r.x + ds::space::kCardPadding;
  const int w = r.width - 2 * ds::space::kCardPadding;
  const int labelH = renderer.getLineHeight(RickyDesignUi::BODY);
  if (!detail || !*detail) {
    RickyDesignUi::text(renderer, rect(x, r.y + (r.height - labelH) / 2, w, labelH), label, RickyDesignUi::BODY,
                        inverted, fui::TextAlign::Left, 1, inverted ? ds::gray::kPaper : tone);
    return;
  }
  const int detailH = renderer.getLineHeight(RickyDesignUi::CAPTION);
  const int top = r.y + (r.height - labelH - detailH - ds::space::kTextStackGap) / 2;
  RickyDesignUi::text(renderer, rect(x, top, w, labelH), label, RickyDesignUi::BODY, inverted, fui::TextAlign::Left, 1,
                      inverted ? ds::gray::kPaper : tone);
  RickyDesignUi::text(renderer, rect(x, top + labelH + ds::space::kTextStackGap, w, detailH), detail,
                      RickyDesignUi::CAPTION, inverted, fui::TextAlign::Left, 1,
                      inverted ? ds::gray::kPaper : (enabled ? ds::gray::kInk2 : ds::gray::kMute));
}
}  // namespace gomokuart
