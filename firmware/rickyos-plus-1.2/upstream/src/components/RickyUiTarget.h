#pragma once

#include <FreeInkUIGfxRenderer.h>

#include <cstdlib>

#include "RickyAaIcons.h"
#include "RickyDesignTokens.h"
#include "RickyGrayStroke.h"

// Firmware-owned adapter: the SDK remains product-independent. A native-sized
// RickyOS line icon uses its coverage twin, including SDK-rendered list rows.
// Other assets, scaling, tiling and rotations keep the SDK's existing contract.
// One renderer reference per host, no scratch bitmap or heap allocation.
class RickyUiTarget : public freeink::ui::DrawTarget {
 public:
  explicit RickyUiTarget(const GfxRenderer& renderer, bool touch = false)
      : target_(renderer, touch), renderer_(renderer) {}

  // GfxRendererTarget is final in the SDK; composition keeps its text metrics,
  // clipping and font-binding API intact without patching or forking the SDK.
  operator freeink::ui::GfxRendererTarget&() { return target_; }
  operator const freeink::ui::GfxRendererTarget&() const { return target_; }
  void setFont(freeink::ui::FontId slot, int id) { target_.setFont(slot, id); }
  void setTextCentering(freeink::ui::TextCentering mode) { target_.setTextCentering(mode); }
  freeink::ui::DeviceContext deviceContext() const { return target_.deviceContext(); }
  freeink::ui::Rect clipRect() const override { return target_.clipRect(); }
  bool setClipRect(freeink::ui::Rect rect) override { return target_.setClipRect(rect); }
  freeink::ui::Size measureText(freeink::ui::FontId font, const char* text,
                                freeink::ui::TextStyle style) const override {
    return target_.measureText(font, text, style);
  }
  int16_t lineHeight(freeink::ui::FontId font) const override { return target_.lineHeight(font); }
  freeink::ui::Rect inkBounds(freeink::ui::FontId font, const char* text, freeink::ui::TextStyle style) const override {
    return target_.inkBounds(font, text, style);
  }
  void fill(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t radius = 0,
            uint8_t corners = freeink::ui::CornersAll) override {
    target_.fill(rect, paint, radius, corners);
  }
  void stroke(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t width, uint8_t radius = 0,
              uint8_t corners = freeink::ui::CornersAll) override {
    // Gray outlines (resting cards, chips) draw in exact product gray on a 16-gray
    // frame instead of the SDK's dither; B/W frames keep the 1-bit fallback.
    if (const auto level = grayLevel(paint); level != ricky::ds::gray::kInk && corners == freeink::ui::CornersAll &&
                                             renderer_.isGrayscale16Active()) {
      RickyGrayStroke::ring(renderer_, rect.x, rect.y, rect.width, rect.height, width, radius, level);
      return;
    }
    target_.stroke(rect, paint, width, radius, corners);
  }
  void line(freeink::ui::Point from, freeink::ui::Point to, uint8_t width, freeink::ui::Paint paint) override {
    // Straight gray rules likewise, the only lines the product draws in gray.
    if (const auto level = grayLevel(paint);
        level != ricky::ds::gray::kInk && renderer_.isGrayscale16Active() && (from.x == to.x || from.y == to.y)) {
      const int x = std::min(from.x, to.x), y = std::min(from.y, to.y);
      const int w = from.y == to.y ? std::abs(to.x - from.x) + 1 : width;
      const int h = from.y == to.y ? width : std::abs(to.y - from.y) + 1;
      renderer_.fillGrayscale16Rect(from.y == to.y ? x : x - width / 2, from.y == to.y ? y - width / 2 : y, w, h,
                                    level);
      return;
    }
    target_.line(from, to, width, paint);
  }
  void triangle(freeink::ui::Point a, freeink::ui::Point b, freeink::ui::Point c, freeink::ui::Paint paint) override {
    target_.triangle(a, b, c, paint);
  }
  void text(freeink::ui::Rect rect, const char* text, freeink::ui::TextStyle style) override {
    // The pinned SDK falls gray text back to solid drawText(). Keep its layout
    // but supply exact product ink to the renderer, never B/W checkerboard text.
    using freeink::ui::Color;
    if (style.color == Color::Transparent) return;
    const auto ink = style.inverted || style.color == Color::White ? ricky::ds::gray::kPaper
                     : style.color == Color::DarkGray              ? ricky::ds::gray::kInk2
                     : style.color == Color::LightGray             ? ricky::ds::gray::kMute
                                                                   : ricky::ds::gray::kInk;
    const GfxRenderer::TextInkScope tone(renderer_, ink);
    if (style.color != Color::White && !style.inverted) style.color = Color::Black;
    if (style.maxLines <= 1) {
      target_.text(rect, text, style);
      return;
    }
    const int height = target_.lineHeight(style.font);
    if (!text || rect.empty() || height <= 0 || rect.height < height) return;
    // Use the same bounded UTF-8 layout that the SDK list measures. Native
    // wrappedText treats an unspaced CJK filename as one oversized word.
    style.maxLines = std::min<int>(style.maxLines, rect.height / height);
    freeink::ui::layoutText(target_, rect, text, style, [&](const char* run, freeink::ui::Rect bounds) {
      auto single = style;
      single.maxLines = 1;
      single.align = freeink::ui::TextAlign::Left;
      target_.text(bounds, run, single);
    });
  }

  void bitmap(const freeink::ui::Rect rect, const freeink::ui::BitmapRef bitmap, const freeink::ui::BitmapMode mode,
              const freeink::ui::Paint foreground = freeink::ui::Paint::solid(freeink::ui::Color::Black),
              const freeink::ui::Rotation rotation = freeink::ui::Rotation::None) override {
    namespace fui = freeink::ui;
    const bool centered = mode == fui::BitmapMode::Center;
    const bool nativeFit =
        mode == fui::BitmapMode::Contain && rect.width == bitmap.width && rect.height == bitmap.height;
    if (bitmap && !rect.empty() && bitmap.format == fui::BitmapFormat::Mask1 && rotation == fui::Rotation::None &&
        (centered || nativeFit) && foreground.kind == fui::PaintKind::Solid &&
        (foreground.color == fui::Color::Black || foreground.color == fui::Color::White)) {
      const auto clip = clipRect();
      const int x = std::max<int>(clip.x, rect.x), y = std::max<int>(clip.y, rect.y);
      const int right = std::min<int>(clip.right(), rect.right()), bottom = std::min<int>(clip.bottom(), rect.bottom());
      const GfxRenderer::ClipScope scope(renderer_, x, y, std::max(0, right - x), std::max(0, bottom - y));
      if (RickyAaIcons::draw(renderer_, bitmap.data, bitmap.width, bitmap.height,
                             rect.x + (rect.width - bitmap.width) / 2, rect.y + (rect.height - bitmap.height) / 2,
                             foreground.color == fui::Color::Black))
        return;
    }
    target_.bitmap(rect, bitmap, mode, foreground, rotation);
  }

 private:
  static ricky::ds::gray::Level grayLevel(const freeink::ui::Paint& paint) {
    if (paint.kind != freeink::ui::PaintKind::Solid) return ricky::ds::gray::kInk;
    return paint.color == freeink::ui::Color::LightGray  ? ricky::ds::gray::kLine
           : paint.color == freeink::ui::Color::DarkGray ? ricky::ds::gray::kInk2
                                                        : ricky::ds::gray::kInk;
  }
  freeink::ui::GfxRendererTarget target_;
  const GfxRenderer& renderer_;
};
