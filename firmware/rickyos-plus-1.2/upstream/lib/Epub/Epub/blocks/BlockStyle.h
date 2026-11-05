#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

// A panel behind a block's lines (CSS background / border / border-radius). The parser
// numbers each decorated element and places it; every line inside carries it, and a page
// draws one rounded panel per run of lines with the same id (closed on each page).
struct BlockBox {
  uint16_t id = 0;         // 0 = no panel
  int16_t left = 0;        // outer left edge, from the content area's left
  int16_t width = 0;       // outer width
  uint8_t padTop = 0;      // padding + border above the first line
  uint8_t padBottom = 0;   // padding + border below the last line
  uint8_t border = 0;      // border width, px
  uint8_t radius = 0;      // corner radius, px
  uint8_t fillLuma = 255;  // 255 = no fill
  uint8_t borderLuma = 0;
  [[nodiscard]] bool decorated() const { return fillLuma < 250 || border > 0; }
};
static_assert(sizeof(BlockBox) == 12, "BlockBox is written to the section cache as is");

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  // Upper bound (in em) for any single side's horizontal margin or padding.
  // Some EPUBs apply huge em-based insets to chapter-opener classes; without a
  // cap, effectiveWidth collapses to 1-2 words per line and justification dumps
  // the remaining space into a single gap.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 2.0f;

  CssTextAlign alignment = CssTextAlign::Justify;

  // Extra pixels between glyphs during layout and drawing.
  int8_t characterSpacing = 0;
  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool isRtl = false;              // true if resolved direction is RTL
  bool directionDefined = false;   // true if direction was explicitly set in CSS/HTML
  // The book's CSS set a top or bottom margin: its spacing is the book's, and the reader's
  // extra paragraph spacing is not added on top. Layout-only, not serialized.
  bool verticalMarginDefined = false;

  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;

  BlockBox box;

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with top margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutTop() const {
    BlockStyle result = *this;
    result.marginTop = 0;
    result.paddingTop = 0;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  // Uses CSS margin collapsing: adjacent parent-child margins resolve to the larger value.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.marginBottom = std::max(marginBottom, source.marginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + source.paddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;
    // Lines inside a panel keep it; a nested panel (placed by the parser) replaces it.
    // Vertical merges an empty block into the next one: the incoming block decides, or the
    // paragraph after a closed panel would join it.
    if (axis == CombineAxis::Horizontal && child.box.id == 0) result.box = box;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
      result.verticalMarginDefined = child.verticalMarginDefined || verticalMarginDefined;
    }

    // Direction is not axis-specific. Inherit from parent when child doesn't define it.
    if (!child.directionDefined && directionDefined) {
      result.isRtl = isRtl;
      result.directionDefined = true;
    }

    // fromBrElement is consumed by startNewTextBlock when an empty <br> block
    // is merged with the following paragraph; never propagate it further.
    result.fromBrElement = false;
    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.verticalMarginDefined = cssStyle.hasMarginTop() || cssStyle.hasMarginBottom();
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.marginLeft = std::min(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.marginRight = std::min(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingLeft = std::min(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.paddingRight = std::min(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the space-width fallback in resolveFirstLineIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    // RTL direction from CSS/HTML
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = (cssStyle.direction == CssTextDirection::Rtl);
      blockStyle.directionDefined = true;
    }
    // Panel look. Placement (id, left, width) needs the parent's insets: the parser does it.
    const int border = cssStyle.hasBorder() ? std::clamp<int>(cssStyle.borderWidth.toPixelsInt16(emSize, vw), 0, 8) : 0;
    const uint8_t fill = cssStyle.hasBackground() ? cssStyle.backgroundLuma : 255;
    if (fill < 250 || border > 0) {
      // Text stays clear of the border.
      blockStyle.paddingTop = static_cast<int16_t>(blockStyle.paddingTop + border);
      blockStyle.paddingBottom = static_cast<int16_t>(blockStyle.paddingBottom + border);
      blockStyle.paddingLeft = static_cast<int16_t>(blockStyle.paddingLeft + border);
      blockStyle.paddingRight = static_cast<int16_t>(blockStyle.paddingRight + border);
      blockStyle.box.border = static_cast<uint8_t>(border);
      blockStyle.box.borderLuma = cssStyle.borderLuma;
      blockStyle.box.fillLuma = fill;
      blockStyle.box.radius = cssStyle.hasBorderRadius()
                                  ? static_cast<uint8_t>(std::clamp<int>(cssStyle.borderRadius.toPixelsInt16(emSize, vw), 0, 40))
                                  : 0;
      blockStyle.box.padTop = static_cast<uint8_t>(std::clamp<int>(blockStyle.paddingTop, 0, 255));
      blockStyle.box.padBottom = static_cast<uint8_t>(std::clamp<int>(blockStyle.paddingBottom, 0, 255));
    }
    return blockStyle;
  }
};
