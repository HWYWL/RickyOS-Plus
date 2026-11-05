#pragma once

#include <Utf8.h>

#include <cstdint>

// Vertical (top-to-bottom, right-to-left) text: how each character sits in its cell.
//
// The layout is the ordinary horizontal one with the viewport turned: a line becomes
// a column and a word's x becomes its distance from the column top. A CJK character's
// horizontal advance is its vertical cell height, so line breaking, justification and
// the line-start rules carry over unchanged. Only drawing differs, per character:
//
//   Upright         ideographs, kana, fullwidth letters and digits, ！？: drawn as is.
//   CornerTopRight  ，。、．: horizontal forms sit low-left in their em box; vertical
//                   setting puts them high-right.
//   Rotated         brackets, quotes, dashes, ellipses, ：；～ and everything non-CJK
//                   (Latin words, digits): turned 90° clockwise, reading downwards.
//                   Turned, 「（《— are exactly their vertical presentation forms.
namespace verticalText {

enum class Placement : uint8_t { Upright, CornerTopRight, Rotated };

constexpr Placement placementFor(const uint32_t cp) {
  switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
      return Placement::CornerTopRight;
    case 0x3008:  // 〈
    case 0x3009:  // 〉
    case 0x300A:  // 《
    case 0x300B:  // 》
    case 0x300C:  // 「
    case 0x300D:  // 」
    case 0x300E:  // 『
    case 0x300F:  // 』
    case 0x3010:  // 【
    case 0x3011:  // 】
    case 0x3014:  // 〔
    case 0x3015:  // 〕
    case 0x3016:  // 〖
    case 0x3017:  // 〗
    case 0x301C:  // 〜
    case 0x3030:  // 〰
    case 0xFF08:  // （
    case 0xFF09:  // ）
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF3B:  // ［
    case 0xFF3D:  // ］
    case 0xFF3F:  // ＿
    case 0xFF5B:  // ｛
    case 0xFF5D:  // ｝
    case 0xFF5E:  // ～
    case 0xFF0D:  // －
    case 0xFF5F:  // ｟
    case 0xFF60:  // ｠
      return Placement::Rotated;
    default:
      break;
  }
  // Dashes, ellipses and curly quotes live outside the CJK blocks and rotate with Latin.
  return utf8IsCjkCodepoint(cp) ? Placement::Upright : Placement::Rotated;
}

// Where an upright ideograph's baseline sits in its cell: the reader faces draw an
// ideograph from 0.80 em above the baseline to 0.12 em below it (measured), so a
// baseline 0.84 em below the cell top centres the ink. Fractions of the cell height.
inline constexpr int kUprightBaselinePercent = 84;
// A turned run is centred on its em box, whose middle sits 0.38 em above the baseline.
inline constexpr int kRotatedCenterPercent = 38;
// CornerTopRight: the em box moves half a cell right and half a cell up.
inline constexpr int kCornerShiftPercent = 50;

// Orientation that turns the reader's frame 90° clockwise: GfxRenderer::Orientation
// steps Portrait -> LandscapeClockwise -> PortraitInverted -> LandscapeCounterClockwise
// by quarter turns, so the turned frame is three steps on. In it, plain left-to-right
// text runs down the page with its tops facing right.
constexpr uint8_t turnedOrientation(const uint8_t orientation) { return static_cast<uint8_t>((orientation + 3) % 4); }

// A point of the reader's frame in the turned frame: x' = y, y' = width - 1 - x.
struct Point {
  int x;
  int y;
};
constexpr Point toTurned(const int x, const int y, const int screenWidth) { return {y, screenWidth - 1 - x}; }

}  // namespace verticalText
