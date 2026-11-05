#include "BaseTheme.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "BleInput.h"
#include "I18n.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/reader/ReaderChrome.h"
#include "components/BatteryStatusLayout.h"
#include "components/HeaderBackTapTarget.h"
#include "components/RickyBrandMark.h"
#include "components/RickyPageUi.h"
#include "components/RickyPowerLayout.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bluetooth.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/customListIcons.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/rickyBatteryIcons.h"
#include "components/icons/rickyPageIcons.h"
#include "components/icons/uiChromeIcons.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "util/TimeUtils.h"

freeink::ui::BitmapRef BaseTheme::checkboxIcon(const bool checked) {
  return freeink::ui::bitmapFromIcon(checked ? icon_checkbox_on_32 : icon_checkbox_off_32);
}

void BaseTheme::drawSplash(const GfxRenderer& renderer, const char* status, const char* version) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  drawRickyPowerScreen(renderer, false, false, version);
  return;
}

void BaseTheme::drawRickyPowerScreen(const GfxRenderer& renderer, const bool sleeping, const bool transition,
                                     const char* version, const uint8_t reveal) {
  renderer.clearScreen();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
  const GfxRenderer::ClipScope clip(renderer, safe.x, safe.y, safe.width, safe.height);
  const int font = UiHighDpiProfile::enabled ? RickyDesignUi::CAPTION : UI_10_FONT_ID;
  const auto layout = RickyPowerLayout::fit(safe, renderer.getLineHeight(font), ricky::ds::space::kBlockGap);
  (void)version;
  (void)reveal;
  constexpr uint8_t phase = 3;
  RickyBrandMark::draw(renderer, layout.mark, phase);
  // One mark + one caption, no ornamental rules or obsolete extra eyebrow.
  // A powered-down display must not imply that a clock keeps updating.
  const char* message = sleeping && transition ? tr(STR_ENTERING_SLEEP) : tr(STR_RICKY_BRAND_TAGLINE);
  UITheme::drawCenteredWrappedText(renderer, layout.message, font, message, 2);
  if (!sleeping || transition) return;  // No version or empty footer decoration on boot.
  UITheme::drawCenteredWrappedText(renderer, layout.footer, font, tr(STR_RICKY_POWERED_OFF), 2);
}

Rect BaseTheme::drawRickyStandbyIndicator(const GfxRenderer& renderer, const bool poweredOff) {
  if (!poweredOff) return Rect{};  // Product standby has no added status chrome.
  const char* label = tr(STR_RICKY_POWERED_OFF);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
  if (safe.width <= 0 || safe.height <= 0) return Rect{};
  const int font = UiHighDpiProfile::enabled ? RickyDesignUi::CAPTION : UI_10_FONT_ID;
  // A quiet status, not a bordered button. Keep a paper backing so it stays
  // readable over arbitrary page/PNG content; no snapshot or extra allocation.
  const int insetX = std::min<int>(ricky::ds::space::k2, safe.width / 8);
  // Align with the retained reader footer and cover its whole percentage slot,
  // rather than clipping half a progress label behind a narrow floating chip.
  const int insetY = std::min<int>(ricky::ds::space::k6, safe.height / 8);
  const int width = std::min(safe.width - insetX * 2,
                             std::max(safe.width / 5, renderer.getTextWidth(font, label) + ricky::ds::space::k4 * 2));
  const int height = std::min(safe.height - insetY * 2, renderer.getLineHeight(font) + ricky::ds::space::k1 * 2);
  const Rect badge{safe.x + safe.width - insetX - width, safe.y + safe.height - insetY - height, width, height};
  const GfxRenderer::ClipScope clip(renderer, badge.x, badge.y, badge.width, badge.height);
  renderer.fillRoundedRect(badge.x, badge.y, badge.width, badge.height,
                           ricky::ds::radius::resolve(ricky::ds::radius::kPill, width, height), Color::White);
  const GfxRenderer::TextInkScope tone(renderer, ricky::ds::gray::kInk2);
  UITheme::drawCenteredWrappedText(renderer, badge, font, label, 1);
  return badge;
}

void BaseTheme::setCheckboxRow(freeink::ui::ListItem& item, const bool checked) {
  item.toggle = true;
  item.value = nullptr;
  item.toggleChecked = checked;
}

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = UiHighDpiProfile::enabled ? UiHighDpiProfile::readerStatusIconSize : 16;
constexpr int bookmarkStatusIconHeight = UiHighDpiProfile::enabled ? UiHighDpiProfile::readerStatusIconSize : 14;
constexpr int bookmarkStatusIconGap = UiHighDpiProfile::enabled ? UiHighDpiProfile::controlGap : 4;
constexpr int bookmarkStatusIconTopCrop = 2;
constexpr int bluetoothStatusIconWidth = UiHighDpiProfile::enabled ? UiHighDpiProfile::readerStatusIconSize : 16;
constexpr int bluetoothStatusIconHeight = UiHighDpiProfile::enabled ? UiHighDpiProfile::readerStatusIconSize : 16;

// Plot only ink pixels; the white bits are transparent. No render-time buffers.
void drawTransparentBitmap(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y,
                           const bool black) {
  for (int row = 0; row < icon.h; ++row) {
    for (int column = 0; column < icon.w; ++column) {
      if ((icon.bits[row * ((icon.w + 7) / 8) + column / 8] & (0x80U >> (column % 8))) == 0) {
        renderer.drawPixel(x + column, y + row, black);
      }
    }
  }
}

void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  drawTransparentBitmap(renderer, icon_reader_bookmark_24, x, y, true);
}

void drawBluetoothStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  drawTransparentBitmap(renderer, icon_reader_bluetooth_24, x, y, true);
}

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  if (battWidth == UiHighDpiProfile::batteryWidth && rectHeight == UiHighDpiProfile::batteryHeight) {
    drawTransparentBitmap(renderer, ricky_battery_body_32x20, x, y, true);
    return;
  }
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawDitherMask(const GfxRenderer& renderer, const int x, const int y, const int width,
                               const int height) {
  for (int py = y; py < y + height; py++) {
    for (int px = x; px < x + width; px++) {
      if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }
  }
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  if (percentage > 100) return;  // Unknown, not a full/empty measured battery.
  const bool charging = gpio.isUsbConnected();
  if (rect.width == UiHighDpiProfile::batteryWidth && rect.height == UiHighDpiProfile::batteryHeight) {
    // The bar sits inside the body with a white gap all round (rickyBatteryIcons.h);
    // any charge left shows at least a sliver.
    constexpr int barWidth = 20;
    const int level = std::min<int>(percentage, 100);
    const int filledWidth = level <= 0 ? 0 : std::max(2, (level * barWidth + 50) / 100);
    if (filledWidth > 0) renderer.fillRect(rect.x + 4, rect.y + 5, filledWidth, 10);
    if (charging) {
      drawTransparentBitmap(renderer, ricky_battery_bolt_halo_32x20, rect.x, rect.y, false);
      drawTransparentBitmap(renderer, ricky_battery_bolt_32x20, rect.x, rect.y, true);
    }
    return;
  }

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  if (percentage > 100) return;  // Initial telemetry pending: do not invent a level or label.
  const int y = UiHighDpiProfile::enabled
                    ? ricky::battery::iconTop(renderer, STATUS_NUMERIC_FONT_ID, rect.y, rect.height)
                    : rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(STATUS_NUMERIC_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y,
                      percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawCoverPlaceholder(const GfxRenderer& renderer, Rect rect) {
  if (rect.width <= 0 || rect.height <= 0) return;
  const int topHeight = rect.height / 3;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.fillRect(rect.x, rect.y + topHeight, rect.width, rect.height - topHeight, true);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  constexpr int ICON_SIZE = 32;
  if (rect.width >= ICON_SIZE + 4 && topHeight >= ICON_SIZE + 4) {
    const int insetX = std::min(24, (rect.width - ICON_SIZE) / 2);
    const int insetY = std::min(24, (topHeight - ICON_SIZE) / 2);
    renderer.drawIcon(CoverIcon, rect.x + insetX, rect.y + insetY, ICON_SIZE);
  }
}

bool BaseTheme::drawCoverThumbFill(const GfxRenderer& renderer, const Bitmap& bitmap, Rect slot, const int xOffset) {
  if (slot.width <= 0 || slot.height <= 0) return false;
  // xOffset nudges the centered art sideways; the clip stays on the slot.
  const int x = slot.x + (slot.width - bitmap.getWidth()) / 2 + xOffset;
  const int y = slot.y + (slot.height - bitmap.getHeight()) / 2;
  const auto clip = renderer.getClipRect();
  const int left = std::max(slot.x, clip[0]);
  const int top = std::max(slot.y, clip[1]);
  renderer.setClipRect(left, top, std::max(0, std::min(slot.x + slot.width, clip[0] + clip[2]) - left),
                       std::max(0, std::min(slot.y + slot.height, clip[1] + clip[3]) - top));
  // Gray thumbnails keep their tones on a 16-level frame; the 2-bit path would dither them.
  const bool drawn = renderer.isGrayscale16Active() && !bitmap.is1Bit()
                         ? renderer.drawBitmapGrayscale16(bitmap, x, y, bitmap.getWidth(), bitmap.getHeight())
                         : renderer.drawBitmap(bitmap, x, y, bitmap.getWidth(), bitmap.getHeight());
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  return drawn;
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage,
                                 const int numericFontId) const {
  const uint16_t percentage = powerManager.getBatteryPercentage();
  if (percentage > 100) return;
  // Align the visible numeric ink, rather than the font's line box/leading.
  const int y = UiHighDpiProfile::enabled
                    ? ricky::battery::iconTop(renderer, numericFontId, rect.y, rect.height)
                    : rect.y + 6;
  const int gap = UiHighDpiProfile::enabled ? UiHighDpiProfile::controlGap : batteryPercentSpacing;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(numericFontId, percentageText.c_str());
    renderer.drawText(numericFontId, rect.x - textWidth - gap, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

// Retain the established theme component interface.
// cppcheck-suppress functionStatic
int BaseTheme::measureProgressBarHeight(const GfxRenderer& renderer, const int barHeight,
                                        const bool showPercentage) const {
  constexpr int percentageGap = 15;
  return barHeight <= 0 ? 0 : barHeight + (showPercentage ? percentageGap + renderer.getLineHeight(UI_10_FONT_ID) : 0);
}

int BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current, const size_t total,
                               const bool showPercentage) const {
  if (total == 0) return rect.y;
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  if (showPercentage) {
    char percentText[16];
    snprintf(percentText, sizeof(percentText), "%d%%", percent);
    renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText);
  }
  return rect.y + measureProgressBarHeight(renderer, rect.height, showPercentage);
}

// Centre a button-hint label inside its box. A label that fits is drawn on the
// single baseline it always was; one too wide used to overflow the button border
// and run into the neighbouring hint, and now wraps to at most two centred lines
// (wrappedText() ellipsises anything that still doesn't fit). Shared so every
// theme's drawButtonHints() gets the same behaviour.
void BaseTheme::drawHintLabel(const GfxRenderer& renderer, const int fontId, const char* label, const int x,
                              const int boxWidth, const int boxTop, const int boxHeight, const int singleLineYOffset) {
  constexpr int textPadding = 4;  // keeps a wrapped label off the button's border
  const int maxTextWidth = boxWidth - (textPadding * 2);

  const int textWidth = renderer.getTextWidth(fontId, label);
  if (textWidth <= maxTextWidth) {
    renderer.drawText(fontId, x + (boxWidth - 1 - textWidth) / 2, boxTop + singleLineYOffset, label);
    return;
  }

  // Spaced by the glyph height, not getLineHeight() — that returns the font's
  // full advanceY (leading included), which stacks two lines taller than the
  // button and clips the second one.
  constexpr int lineGap = 2;
  const int step = renderer.getTextHeight(fontId) + lineGap;
  const auto lines = renderer.wrappedText(fontId, label, maxTextWidth, 2);
  const int block = static_cast<int>(lines.size()) * step - lineGap;
  int lineY = boxTop + std::max(1, (boxHeight - block) / 2);
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(fontId, line.c_str());
    renderer.drawText(fontId, x + (boxWidth - 1 - lineWidth) / 2, lineY, line.c_str());
    lineY += step;
  }
}

void BaseTheme::drawButtonHintsWithStyle(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                         const char* btn4, const bool upstreamStyle) const {
  if (upstreamStyle && SETTINGS.uiTheme == CrossPointSettings::INX) {
    static const LyraTheme theme;
    theme.drawButtonHints(renderer, btn1, btn2, btn3, btn4);
    return;
  }
  drawButtonHints(renderer, btn1, btn2, btn3, btn4);
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (!buttonHintsVisible()) return;

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      // Zero gray-plane bits leave the monochrome hint from the base pass intact.
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, grayscale);
      if (grayscale) continue;
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      drawHintLabel(renderer, UI_10_FONT_ID, labels[i], x, buttonWidth, pageHeight - buttonY, buttonHeight,
                    textYOffset);
    }
  }

  renderer.setOrientation(orig_orientation);
}

// Retain the established theme component interface.
// cppcheck-suppress functionStatic
bool BaseTheme::buttonHintsVisible() const { return !gpio.hasTouch() && SETTINGS.showButtonHints; }

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (gpio.hasEdgeSideButtons()) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

int BaseTheme::getListRowStep(bool hasSubtitle) const {
  const int rowStep = listRowStep_[hasSubtitle].load(std::memory_order_relaxed);
  if (rowStep > 0) return rowStep;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int height = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return height + metrics.listRowGap;
}

int BaseTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  const int rowStep = getListRowStep(hasSubtitle);
  if (rowStep <= 0) return 1;
  const int gap = std::max(BoardConfig::hasTouch() ? 6 : 0, UITheme::getInstance().getMetrics().listRowGap);
  return std::max(1, (contentHeight + gap) / rowStep);
}

// Retain the established theme component interface.
// cppcheck-suppress functionStatic
void BaseTheme::drawSideScrollBar(const GfxRenderer& renderer, Rect rect, const int itemCount, const int pageStartIndex,
                                  const int pageItems) const {
  if (itemCount <= pageItems || pageItems <= 0 || rect.height <= 0) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int totalPages = 1 + (itemCount - 1) / pageItems;
  const int currentPage = std::clamp(pageStartIndex / pageItems, 0, totalPages - 1);
  const int barHeight = std::max(1, static_cast<int>((static_cast<int64_t>(rect.height) * pageItems) / itemCount));
  const int barY =
      rect.y + static_cast<int>((static_cast<int64_t>(rect.height - barHeight) * currentPage) / (totalPages - 1));
  const int barX = rect.x + rect.width - metrics.scrollBarRightOffset;
  renderer.drawLine(barX, rect.y, barX, rect.y + rect.height, true);
  renderer.fillRect(barX - metrics.scrollBarWidth, barY, metrics.scrollBarWidth, barHeight, true);
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed, const bool showSelection,
                         const std::function<bool(int index)>&) const {
  if (itemCount <= 0) return;
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  applyUiTextAlignment(ui.target);
  const auto& tokens = refreshSharedUiThemeTokens(ui.target);
  fui::Screen<1> screen(ui.frame, tokens);
  screen.setContentMarginFromScreen(
      fui::Insets{fui::clampI16(rect.y), fui::clampI16(renderer.getScreenWidth() - rect.x - rect.width),
                  fui::clampI16(renderer.getScreenHeight() - rect.y - rect.height), fui::clampI16(rect.x)});

  // Reuse the legacy callbacks and three per-row strings; never materialize
  // a list. Their storage is released when this synchronous render returns.
  struct Rows {
    const std::function<std::string(int)>& title;
    const std::function<std::string(int)>& subtitle;
    const std::function<std::string(int)>& value;
    const std::function<bool(int)>& dimmed;
    const std::function<UIIcon(int)>& icon;
    std::string titleText, subtitleText, valueText;
  } rows{rowTitle, rowSubtitle, rowValue, rowDimmed, rowIcon, {}, {}, {}};
  // ListProps embeds styles (~784 bytes on host); keep render-only scratch
  // outside the C3 render-task stack. Reset all fields for every invocation.
  static fui::ListProps props;
  props = {};
  props.rowProviderCtx = &rows;
  props.rowProvider = [](void* ctx, uint16_t index, fui::ListItem& item) {
    auto& row = *static_cast<Rows*>(ctx);
    row.titleText = row.title(index);
    row.subtitleText = row.subtitle ? row.subtitle(index) : std::string{};
    row.valueText = row.value ? row.value(index) : std::string{};
    item.label = row.titleText.c_str();
    item.subtitle = row.subtitleText.empty() ? nullptr : row.subtitleText.c_str();
    item.value = row.valueText.empty() ? nullptr : row.valueText.c_str();
    item.enabled = !row.dimmed || !row.dimmed(index);
    if (row.icon) item.icon = listIconFor(row.icon(index));
  };
  props.count = itemCount;
  props.selectedIndex = showSelection ? selectedIndex : -1;
  props.labelText = tokens.smallText;
  props.labelText.maxLines = 1;
  props.subtitleText = tokens.smallText;
  props.subtitleText.maxLines = 1;
  props.valueInset = 8;
  props.iconSize = rowIcon ? 24 : 0;
  props = screen.resolveListProps(props);
  if (rowSubtitle) props.rowHeight += ui.target.lineHeight(props.subtitleText.font);
  listRowStep_[rowSubtitle != nullptr].store(props.rowHeight + props.rowGap, std::memory_order_relaxed);
  const int pageItems = getListPageItems(rect.height, rowSubtitle != nullptr);
  props.topIndex = std::max(0, selectedIndex) / pageItems * pageItems;
  // Existing callers own input and paginate by fixed rows; do not draw an
  // untappable partial trailing row or wrap beyond those same hit bands.
  props.partialTrailingRow = false;
  fui::ListNav nav;
  nav.top = props.topIndex;
  nav.selected = selectedIndex;
  props.nav = &nav;
  screen.list(props);
}

// Slightly inside the side padding: the battery's boxed glyph and the clock
// digits read wider than text/cover ink on the same line, so flush placement
// looks like it overhangs the content columns.
int BaseTheme::headerStatusInset(bool upstreamStyle) { return uiThemeMetrics(upstreamStyle).headerSidePadding + 4; }

void BaseTheme::applyHeaderStatus(const GfxRenderer& renderer, freeink::ui::HeaderProps& props, bool upstreamStyle) {
  const ThemeMetrics& metrics = uiThemeMetrics(upstreamStyle);
  auto& status = props.status;

  // Status text stays at the fixed small font on every screen: FONT_LABEL is
  // bound to SMALL_FONT_ID by makeUiTarget() (FUI screens) and drawHeader()
  // (passive frames), while the uiScale FONT_SMALL is for list subtitles.
  status.battery.text.font = freeink::ui::GfxRendererTarget::FONT_LABEL;

  const uint16_t percentage = powerManager.getBatteryPercentage();
  status.showBattery = percentage <= 100;
  status.battery.percent = static_cast<uint8_t>(percentage > 100 ? 0 : percentage);
  status.battery.charging = gpio.isUsbConnected();
  // Static label buffers: headers draw on the single render task, and the
  // strings only need to outlive the fui::header() call that consumes them.
  static char percentText[8];
  status.battery.label = "";
  if (percentage <= 100 &&
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS) {
    snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(percentage));
    status.battery.label = percentText;
  }
  status.battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  status.battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  status.battery.gap = batteryPercentSpacing;
  status.batteryLeft = metrics.headerBatterySide == 1;
  status.edgeInset = static_cast<int16_t>(headerStatusInset(upstreamStyle));

  // Header chrome geometry. Status lives on the theme's thin top strip in
  // fixed corners — battery top-right, a corner clock (headerClockCentered =
  // false) top-left, a centered clock top-center — and never repositions.
  // The content row (title, back arrow, trailing action buttons) centers on
  // the region between the strip and the band bottom, so its padding reads
  // as balanced under the status line rather than against the full band.
  // Text hangs low in its line cell by the font's internal leading; the icon
  // buttons drop by that amount to align with the glyphs the user sees.
  // Buttons keep a standard square (a band-8 button dwarfs its 24px icon);
  // the boxes are invisible and minTouchSize pads the tap target.
  constexpr int16_t headerButtonSize = 48;
  const int16_t bandHeight = static_cast<int16_t>(metrics.headerHeight);
  const int16_t strip = static_cast<int16_t>(metrics.batteryBarHeight);
  const int titleFontId = uiScaleSpec(upstreamStyle).titleFontId;
  const int16_t opticalDrop =
      static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  props.leadingSize = headerButtonSize;
  props.trailingSize = headerButtonSize;
  props.actionOffsetY = static_cast<int16_t>(strip + (bandHeight - strip - headerButtonSize) / 2 - 4 + opticalDrop);
  // Shift the title's band-centered box down by half the strip: its center
  // lands on the below-strip region's midline with the buttons.
  props.titleOffsetY = static_cast<int16_t>(strip / 2);
  status.stripHeight = strip;
  status.clockCentered = metrics.headerClockCentered;

  // Header clock, opposite the battery, on every screen that draws this band
  // (SETTINGS.clockShowInHeader). Themes whose title layout has no room for
  // the clock's left reserve opt out via headerShowsClock.
  static char clockText[10];
  if (metrics.headerShowsClock && SETTINGS.clockShowInHeader &&
      TimeUtils::formatCurrentTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1)) {
    status.clockText = clockText;
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool backButton) const {
  drawHeaderWithStyle(renderer, rect, title, subtitle, backButton, false);
}

void BaseTheme::drawHeaderWithStyle(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                                    const bool backButton, const bool upstreamStyle, const int trailingReserve) {
  if (UiHighDpiProfile::enabled && SETTINGS.uiTheme == CrossPointSettings::INX && backButton && title) {
    namespace ds = ricky::ds;
    namespace fui = freeink::ui;
    const int statusHeight = std::min<int>(ds::layout::kStatusBarHeight, rect.height);
    GUI.drawMainTabStatusBar(renderer, Rect{rect.x, rect.y, rect.width, statusHeight});
    const int rowY = rect.y + statusHeight;
    const int rowHeight = std::max(0, rect.height - statusHeight);
    const int left = rect.x + ds::space::kPageMargin;
    auto target = makeUiTarget(renderer);
    RickyPageUi::pageIcon(target, renderer, left, rowY + rowHeight / 2, icon_ricky_back_40);
    const int titleX = left + 40 + ds::space::kInlineGap;
    int titleRight = rect.x + rect.width - ds::space::kPageMargin - std::max(0, trailingReserve);
    if (subtitle && *subtitle) {
      const int font = RickyDesignUi::fontFor(renderer, RickyDesignUi::CAPTION, subtitle);
      const int width = std::min(renderer.getTextWidth(font, subtitle), std::max(0, rect.width / 3));
      RickyDesignUi::text(renderer,
                          {static_cast<int16_t>(titleRight - width), static_cast<int16_t>(rowY),
                           static_cast<int16_t>(width), static_cast<int16_t>(rowHeight)},
                          subtitle, font, false, fui::TextAlign::Right);
      titleRight -= width + ds::space::kInlineGap;
    }
    RickyDesignUi::text(renderer,
                        {static_cast<int16_t>(titleX), static_cast<int16_t>(rowY),
                         static_cast<int16_t>(std::max(0, titleRight - titleX)), static_cast<int16_t>(rowHeight)},
                        title, RickyDesignUi::HEADLINE);
    HeaderBackTapTarget::setLeading(rect.x, rowY, rect.width, rowHeight, titleX - rect.x + 80);
    return;
  }
  // Every activity header renders through the FreeInkUI header + battery
  // indicator components, styled by the active theme's tokens (padding,
  // centering, underline). Non-interactive frame: no hit rects registered.
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec(upstreamStyle);
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  applyUiTextAlignment(ui.target, upstreamStyle);
  // Refresh the app-wide shared tokens instead of copying ~1.5KB of
  // ThemeTokens onto this render-path stack frame; the values derived here
  // are identical to what every FreeInkApp screen derives. Goes through the
  // same publish-a-fresh-slot path applySharedUiTheme() uses (see
  // UiAppHelpers.h) rather than overwriting the previously-published
  // instance in place, since some other FreeInkApp could be mid-read of it.
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(ui.target, upstreamStyle);
  // Header status text (battery percent, right label) stays at the fixed
  // small font like the legacy headers; the uiScale small font is for list
  // subtitles.
  ui.target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  ui.target.setFont(fui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
  const fui::Rect band{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y), static_cast<int16_t>(rect.width),
                       static_cast<int16_t>(rect.height)};

  fui::HeaderProps props;
  props.title = title;
  props.rightLabel = subtitle;  // firmware headers right-align the secondary text
  // Battery + clock chrome and their title reserves live in the FreeInkUI
  // header component; this only fills the values from settings and metrics.
  applyHeaderStatus(renderer, props, upstreamStyle);
  if (rect.height < uiThemeMetrics(upstreamStyle).headerHeight) {
    // Short bands (home) are not split into strip + content row: the title
    // centers on the band, clear of the band's bottom edge.
    props.titleOffsetY = 0;
  }
  // Tappable back button leading the band on touch boards, so every pushed
  // screen offers a visible way out beside the edge-swipe gesture. This frame
  // registers no hit rects, so the rect is recorded in HeaderBackTapTarget and
  // MappedInputManager folds taps on it into Button::Back. Same geometry as
  // the FUI header's leading slot (applyHeaderStatus set the size/offset) so
  // the recorded rect matches the drawn button.
  const int16_t backBtnSize = props.leadingSize;
  const bool showBackButton = backButton && title != nullptr && gpio.hasTouch();
  if (showBackButton) {
    props.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    props.leadingAction = 1;  // any non-NO_ACTION id: paints the button, routing is via HeaderBackTapTarget
    const int titleWidth = renderer.getTextWidth(uiScaleSpec(upstreamStyle).titleFontId, title);
    HeaderBackTapTarget::setLeading(band.x, band.y, band.width, band.height, 4 + backBtnSize + titleWidth + 24);
  } else {
    HeaderBackTapTarget::clear();
  }
  props.borderEdges = fui::EdgeBottom;
  props.titleText = tokens.titleText;
  props.titleText.align = tokens.headerTitleAlign;
  props.subtitleText = tokens.smallText;
  props.styles = tokens.popup;
  props.sidePadding = tokens.headerSidePadding;
  // Underline only under a titled header: an untitled band (Lyra home screen)
  // historically drew no rule, and the old themes keyed the line on the title.
  if (title != nullptr && props.styles.normal.border.kind == fui::PaintKind::None && tokens.headerUnderline > 0) {
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = tokens.headerUnderline;
  }
  fui::header(ui.frame, band, props);
}

void BaseTheme::drawMainTabBar(const GfxRenderer&, Rect, MainTab) const {}
void BaseTheme::drawMainTabStatusBar(const GfxRenderer&, Rect) const {}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int labelGap = 10;
  const int contentWidth = std::max(0, rect.width - BaseMetrics::values.contentSidePadding * 2);

  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - labelGap);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(UI_12_FONT_ID, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, rect.y, truncatedLabel.c_str(),
                      true, EpdFontFamily::REGULAR);
  }
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           const bool selected) const {
  constexpr int underlineHeight = 2;
  constexpr int underlineGap = 4;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  for (const auto& tab : tabs) {
    const auto style = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, tab.label, style);
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected), style);
    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

bool BaseTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height) return false;
  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  for (size_t i = 0; i < tabs.size(); i++) {
    const auto& tab = tabs[i];
    const auto style = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, tab.label, style);
    const int left = i == 0 ? rect.x : currentX - BaseMetrics::values.tabSpacing / 2;
    const int right = currentX + textWidth + BaseMetrics::values.tabSpacing / 2;
    if (x >= left && x < right) {
      index = static_cast<int>(i);
      return true;
    }
    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
  return false;
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
int BaseTheme::recentBookIndexAt(int, int) const {
  // Single-cover themes show only the first recent book; a tap anywhere in
  // the cover strip therefore selects book 0.
  return 0;
}

void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // The card matches the cover aspect except when width-capped; fill
          // the card 1:1 and crop the overflow rather than rescale the dither.
          drawCoverThumbFill(renderer, bitmap, Rect{bookX, bookY, bookWidth, bookHeight});

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

int BaseTheme::getMenuRowHeight(const GfxRenderer&) const { return UITheme::getInstance().getMetrics().menuRowHeight; }

BaseTheme::MenuRowGeometry BaseTheme::getMenuRowGeometry(const GfxRenderer&, const Rect& rect, const int,
                                                         const int rowCount) const {
  // Mirror of drawButtonMenu: rows start below the rect at the vertical
  // spacing offset and step by row height + spacing, spanning the content
  // width between the side paddings.
  const auto& m = BaseMetrics::values;
  return {rect.y + m.verticalSpacing,    m.menuRowHeight + m.menuSpacing,           m.menuRowHeight, 0, rowCount,
          rect.x + m.contentSidePadding, rect.x + rect.width - m.contentSidePadding};
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, const int buttonCount, const int selectedIndex,
                               const std::function<std::string(int)>& buttonLabel,
                               const std::function<UIIcon(int)>& rowIcon, const int rowSpacing) const {
  const int spacing = rowSpacing < 0 ? BaseMetrics::values.menuSpacing : rowSpacing;
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + spacing);
    const bool selected = selectedIndex == i;
    const Rect tile{rect.x + BaseMetrics::values.contentSidePadding, tileY,
                    rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight};
    if (selected)
      renderer.fillRect(tile.x, tile.y, tile.width, tile.height);
    else
      renderer.drawRect(tile.x, tile.y, tile.width, tile.height);
    const std::string label = buttonLabel(i);
    const int textX = rect.x + (rect.width - renderer.getTextWidth(UI_10_FONT_ID, label.c_str())) / 2;
    const int textY = tileY + (tile.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str(), !selected);
  }
}

void BaseTheme::drawHomeMenu(GfxRenderer& renderer, Rect rect, const int buttonCount, const int selectedIndex,
                             const std::function<std::string(int)>& buttonLabel,
                             const std::function<UIIcon(int)>& rowIcon) const {
  drawButtonMenu(renderer, rect, buttonCount, selectedIndex, buttonLabel, rowIcon);
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  return drawPopupBox(renderer, message, 0);
}

Rect BaseTheme::drawProgressPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return drawPopupBox(renderer, message, metrics.popupProgressBarHeight + metrics.popupMarginY);
}

Rect BaseTheme::popupProgressBar(const Rect& layout) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  return Rect{barX, barY, barWidth, barHeight};
}

Rect BaseTheme::drawPopupBox(const GfxRenderer& renderer, const char* message, const int extraHeight) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2 + extraHeight;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, metrics.popupTextInverted, popupFontFamily);
  if (extraHeight > 0) {
    // The empty track, in the fill's tone, so 0% already shows the whole length.
    const Rect bar = popupProgressBar(Rect{x, y, w, h});
    if (bar.width > 2 && bar.height > 0)
      renderer.drawRect(bar.x, bar.y, bar.width, bar.height, 1, metrics.popupProgressFillInverted);
  }
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect bar = popupProgressBar(layout);
  const int barHeight = bar.height, barWidth = bar.width, barX = bar.x, barY = bar.y;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawRickyReaderChrome(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                                      const int pageCount, const char* title, const bool bookmarked,
                                      const bool estimated, const bool header, const Rect* area) {
  namespace ds = ricky::ds;
  const auto sb = SETTINGS.statusBarSpec();
  const Rect safe = area ? *area : UITheme::getInstance().getScreenSafeArea(renderer);
  const int left = safe.x + ds::layout::kReaderChromeInset;
  const int right = safe.x + safe.width - ds::layout::kReaderChromeInset;
  if (right <= left) return;
  const GfxRenderer::ClipScope clip(renderer, safe.x, safe.y, safe.width, safe.height);
  const auto chrome = readerChrome::compactLayout(
      std::max(renderer.getLineHeight(RickyDesignUi::CAPTION), renderer.getLineHeight(STATUS_NUMERIC_FONT_ID)),
      sb.headerLaneVisible(), sb.footerLaneVisible(), sb.showsProgressBar(), sb.progressBarHeightPx);
  // Bookmark and Bluetooth marks lead the footer row; with the footer hidden they lead
  // the header instead, since the text then runs down to where the footer was.
  const bool marksInHeader = !sb.footerLaneVisible();
  if (sb.headerLaneVisible() && header) {
    const int y = safe.y + chrome.headerTextY;
    int titleLeft = left, titleRight = right;
    if (marksInHeader && bleinput::isConnected()) {
      drawBluetoothStatusIcon(renderer, titleLeft, y + 4);
      titleLeft += bluetoothStatusIconWidth + ds::space::k2;
    }
    if (marksInHeader && bookmarked) {
      drawBookmarkStatusIcon(renderer, titleLeft, y + 4);
      titleLeft += bookmarkStatusIconWidth + ds::space::k2;
    }
    // Retain clock visibility/12-hour policy. Its old side setting remains
    // meaningful: a right clock precedes the battery, a left clock leads.
    if (sb.showBattery) {
      int width = UiHighDpiProfile::batteryWidth;
      if (sb.showBatteryPercent) {
        char percent[8];
        snprintf(percent, sizeof(percent), "%u%%", powerManager.getBatteryPercentage());
        width += batteryPercentSpacing + renderer.getTextWidth(STATUS_NUMERIC_FONT_ID, percent);
      }
      titleRight -= width;
      GUI.drawBatteryLeft(renderer,
                          Rect{titleRight, y + 4, UiHighDpiProfile::batteryWidth, UiHighDpiProfile::batteryHeight},
                          sb.showBatteryPercent);
      titleRight -= ds::space::k4;
    }
    if (sb.showsClock()) {
      char time[9];
      if (!TimeUtils::formatCurrentTime(time, sizeof(time), sb.clock12h)) snprintf(time, sizeof(time), "--:--");
      const int width = renderer.getTextWidth(STATUS_NUMERIC_FONT_ID, time);
      const bool clockRight = sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT;
      const int x = clockRight ? titleRight - width : titleLeft;
      renderer.drawText(STATUS_NUMERIC_FONT_ID, x, y, time);
      if (clockRight)
        titleRight = x - ds::space::k4;
      else
        titleLeft += width + ds::space::k4;
    }
    if (title && *title && titleRight > titleLeft) {
      RickyDesignUi::secondaryText(renderer,
                                   {static_cast<int16_t>(titleLeft), static_cast<int16_t>(y),
                                    static_cast<int16_t>(titleRight - titleLeft), static_cast<int16_t>(chrome.rowHeight)},
                                   title, RickyDesignUi::CAPTION, false, freeink::ui::TextAlign::Center);
    }
  }
  // The footer row's free span, between the page count and the percent: the bar's place.
  int barLeft = left, barRight = right;
  if (sb.footerLaneVisible()) {
    const int textY = safe.y + safe.height - chrome.footerTextBottom;
    int counterLeft = left;
    if (bleinput::isConnected()) {
      drawBluetoothStatusIcon(renderer, counterLeft, textY + 4);
      counterLeft += bluetoothStatusIconWidth + ds::space::k2;
    }
    if (bookmarked) {
      drawBookmarkStatusIcon(renderer, counterLeft, textY + 4);
      counterLeft += bookmarkStatusIconWidth + ds::space::k2;
    }
    if (sb.showChapterPageCount) {
      char pages[32];
      snprintf(pages, sizeof(pages), "%s%d / %d", estimated ? "~" : "", currentPage, pageCount);
      RickyDesignUi::secondaryText(renderer,
                                   {static_cast<int16_t>(counterLeft), static_cast<int16_t>(textY),
                                    static_cast<int16_t>((right - counterLeft) / 2), static_cast<int16_t>(chrome.rowHeight)},
                                   pages, RickyDesignUi::CAPTION);
      counterLeft += renderer.getTextWidth(RickyDesignUi::fontFor(renderer, RickyDesignUi::CAPTION, pages), pages);
    }
    barLeft = counterLeft == left ? left : counterLeft + ds::space::k4;
    if (sb.showBookProgressPercent) {
      char percent[12];
      snprintf(percent, sizeof(percent), "%.0f%%", std::clamp(bookProgress, 0.0f, 100.0f));
      RickyDesignUi::secondaryText(renderer,
                                   {static_cast<int16_t>(left), static_cast<int16_t>(textY),
                                    static_cast<int16_t>(right - left), static_cast<int16_t>(chrome.rowHeight)},
                                   percent, RickyDesignUi::CAPTION, false, freeink::ui::TextAlign::Right);
      barRight = right - renderer.getTextWidth(RickyDesignUi::fontFor(renderer, RickyDesignUi::CAPTION, percent),
                                               percent) - ds::space::k4;
    }
  }
  if (sb.showsProgressBar()) {
    const int height = std::max<int>(1, sb.progressBarHeightPx);
    const int percent = sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS
                            ? static_cast<int>(bookProgress)
                        : pageCount > 0 ? currentPage * 100 / pageCount
                                        : 0;
    if (barRight > barLeft) {
      // Only the Plus reader re-pulses unchanged gray toward black; elsewhere the gray
      // track reads fine and stays.
#if FREEINK_DEVICE_METALIO_PLUS
      constexpr bool plusReaderLineTrack = true;
#else
      constexpr bool plusReaderLineTrack = false;
#endif
      RickyDesignUi::progress(
          renderer,
          {static_cast<int16_t>(barLeft),
           static_cast<int16_t>(safe.y + safe.height - height - chrome.progressBottom),
           static_cast<int16_t>(barRight - barLeft), static_cast<int16_t>(height)},
          percent, false, plusReaderLineTrack);
    }
  }
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin, const bool isPageBookmarked, const bool pageCountEstimated) {
#if FREEINK_DEVICE_READPICO
  constexpr int textFontId = READER_STATUS_FONT_ID;
  constexpr int estimateFontId = READER_ESTIMATE_FONT_ID;
#else
  constexpr int textFontId = SMALL_FONT_ID;
  constexpr int estimateFontId = UI_10_FONT_ID;
#endif
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto sb = SETTINGS.statusBarSpec();
  const bool showStatusBarTextLane = sb.textLaneVisible();

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom;
  textY += UITheme::getStatusBarTextTopPadding(renderer);
#if !FREEINK_DEVICE_READPICO
  textY -= 4;
#endif

  int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  int rightClusterX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (sb.showBookProgressPercent || sb.showChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Draw the estimate marker separately so it can use the next UI font size.
    const bool showEstimate = pageCountEstimated && sb.showChapterPageCount;

    if (sb.showBookProgressPercent && sb.showChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (sb.showBookProgressPercent) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }

    int progressTextWidth = renderer.getTextWidth(textFontId, progressStr);
    const int estimateWidth = showEstimate ? renderer.getTextWidth(estimateFontId, "~") : 0;
    constexpr int estimateGap = 2;
    const int estimateSpacing = showEstimate ? estimateGap : 0;
    const int progressX = rightClusterX - estimateWidth - estimateSpacing - progressTextWidth;
    if (showEstimate) {
      const int estimateY = textY + (renderer.getLineHeight(textFontId) - renderer.getLineHeight(estimateFontId)) / 2;
      renderer.drawText(estimateFontId, progressX, estimateY, "~");
    }
    renderer.drawText(textFontId, progressX + estimateWidth + estimateSpacing, textY, progressStr);

    rightClusterWidth += estimateWidth + estimateSpacing + progressTextWidth;
  }

  // Draw Progress Bar
  if (sb.showsProgressBar()) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = renderer.getScreenWidth() - barMarginLeft - barMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom - sb.progressBarHeightPx -
                             paddingBottom + (fillMargin ? 1 : 0);
    size_t progress;
    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int barHeight = sb.progressBarHeightPx + (fillMargin ? orientedMarginBottom - 1 : 0);
    renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
  }

  // Draw Battery
  const bool showBatteryPercentage = sb.showBatteryPercent;

  if (sb.showBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{leftClusterX + leftClusterWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    int batteryWidth = metrics.batteryWidth;

    if (showBatteryPercentage) {
      const uint16_t percentage = powerManager.getBatteryPercentage();
      // width of icon + spacing + text for layout purposes
      batteryWidth += batteryPercentSpacing +
                      renderer.getTextWidth(STATUS_NUMERIC_FONT_ID, (std::to_string(percentage) + "%").c_str());
    }

    leftClusterWidth += batteryWidth;
  }

  // Draw the system clock on every board; an external RTC is optional.
  if (sb.showsClock()) {
    char timeBuf[9];
    if (!TimeUtils::formatCurrentTime(timeBuf, sizeof(timeBuf), sb.clock12h)) {
      snprintf(timeBuf, sizeof(timeBuf), "--:--");
    }
    int clockTextWidth = renderer.getTextWidth(STATUS_NUMERIC_FONT_ID, timeBuf);
    int clockX = 0;
    if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {
      clockX = leftClusterX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);
      leftClusterWidth += clockTextWidth + 10;
    } else if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {
      clockX = rightClusterX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockTextWidth;
      rightClusterWidth += clockTextWidth + 10;
    }
    renderer.drawText(STATUS_NUMERIC_FONT_ID, clockX, textY, timeBuf);
  }

  if (showStatusBarTextLane && bleinput::isConnected()) {
    const int bluetoothGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    drawBluetoothStatusIcon(renderer, leftClusterX + leftClusterWidth + bluetoothGap, textY + 3);
    leftClusterWidth += bluetoothStatusIconWidth + bluetoothGap;
  }

  // Draw Bookmark
  if (showStatusBarTextLane && isPageBookmarked) {
    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    const int bookmarkX = leftClusterX + leftClusterWidth + bookmarkGap;
    const int bookmarkY = textY + 5;
    drawBookmarkStatusIcon(renderer, bookmarkX, bookmarkY);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(textFontId, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (UiHighDpiProfile::enabled && availableTitleSpace <= 0) return;
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(textFontId, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(textFontId, title.c_str());
    }

    renderer.drawText(textFontId,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

// Retain the established theme component interface.
// cppcheck-suppress functionStatic
bool BaseTheme::drawSelectionBackground(const GfxRenderer& renderer, const Rect rect) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, metrics.optionPopupSelectionRadius,
                           metrics.optionPopupSelectionLight ? Color::LightGray : Color::Black);
  return metrics.optionPopupSelectionLight;
}

void BaseTheme::drawActionButton(const GfxRenderer& renderer, const Rect rect, const char* label,
                                 const bool active) const {
  if (rect.width <= 0 || rect.height <= 0 || !label || !*label) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  bool foregroundBlack = true;
  if (active) {
    foregroundBlack = drawSelectionBackground(renderer, rect);
  } else {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, metrics.optionPopupSelectionRadius, Color::White);
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, metrics.optionPopupSelectionRadius, true);
  }
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  const int textX = rect.x + (rect.width - textWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, textX, textY, label, foregroundBlack, EpdFontFamily::BOLD);
}

void BaseTheme::drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                                int selectedIndex, const char* /*message*/) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int optionFontId = metrics.optionPopupUseSmallFont ? UI_10_FONT_ID : UI_12_FONT_ID;
  const EpdFontFamily::Style optionStyle =
      metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

  const int itemSpacing = metrics.optionPopupItemSpacing;
  const int innerPadding = metrics.optionPopupInnerPadding;
  const int selectionHPadding = metrics.optionPopupSelectionHPadding;
  const int selectionVPadding = metrics.optionPopupSelectionVPadding;

  const int optionLineHeight = renderer.getLineHeight(optionFontId);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = optionLineHeight + selectionVPadding * 2;

  int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
  for (const auto& opt : options) {
    int w = renderer.getTextWidth(optionFontId, opt.c_str(), optionStyle);
    if (w > maxTextWidth) maxTextWidth = w;
  }

  const int optionCount = static_cast<int>(options.size());
  const int listHeight = rowHeight * optionCount + itemSpacing * (optionCount - 1);
  const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2) * 12 / 10,
                               pageWidth - metrics.optionPopupDialogSideMargin * 2);
  const int contentHeight = titleLineHeight + metrics.optionPopupTitleGap + listHeight;
  const int dialogH = contentHeight + innerPadding * 2;
  const int dialogX = (pageWidth - dialogW) / 2;
  const int dialogY = (pageHeight - dialogH) / 2;

  const int frameThickness = metrics.popupFrameThickness;
  const int frameRadius = metrics.popupCornerRadius;

  if (frameRadius > 0) {
    renderer.fillRoundedRect(dialogX - frameThickness, dialogY - frameThickness, dialogW + frameThickness * 2,
                             dialogH + frameThickness * 2, frameRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(dialogX, dialogY, dialogW, dialogH, frameRadius, Color::Black);
    renderer.fillRoundedRect(dialogX + frameThickness, dialogY + frameThickness, dialogW - frameThickness * 2,
                             dialogH - frameThickness * 2,
                             frameRadius - frameThickness > 0 ? frameRadius - frameThickness : 0, Color::White);
  } else {
    renderer.fillRect(dialogX - frameThickness, dialogY - frameThickness, dialogW + frameThickness * 2,
                      dialogH + frameThickness * 2, true);
    renderer.fillRect(dialogX, dialogY, dialogW, dialogH, false);
  }

  int y = dialogY + innerPadding;

  renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
  y += titleLineHeight;

  if (metrics.optionPopupTitleSeparator) {
    const int sepY = y + metrics.optionPopupTitleGap / 2;
    renderer.drawLine(dialogX + innerPadding, sepY, dialogX + dialogW - innerPadding, sepY, true);
  }

  y += metrics.optionPopupTitleGap;

  const int itemRectX = dialogX + innerPadding;
  const int itemRectW = dialogW - innerPadding * 2;
  const int selectionRadius = metrics.optionPopupSelectionRadius;

  for (int i = 0; i < optionCount; i++) {
    const int itemY = y + i * (rowHeight + itemSpacing);
    const bool selected = (i == selectedIndex);
    const char* labelText = options[i].c_str();

    if (metrics.optionPopupDrawAllRows || selected) {
      Color rowColor;
      if (selected) {
        rowColor = metrics.optionPopupSelectionLight ? Color::LightGray : Color::Black;
      } else {
        rowColor = Color::White;
      }
      if (selectionRadius > 0) {
        renderer.fillRoundedRect(itemRectX, itemY, itemRectW, rowHeight, selectionRadius, rowColor);
      } else {
        renderer.fillRect(itemRectX, itemY, itemRectW, rowHeight, rowColor == Color::Black);
      }
    }

    const int textW = renderer.getTextWidth(optionFontId, labelText, optionStyle);
    const int textY = itemY + (rowHeight - optionLineHeight) / 2;
    const int textX = itemRectX + (itemRectW - textW) / 2;
    // Unselected items: text is dark (invert=true means draw on white bg).
    // Selected on dark bg: text must be white (invert=false).
    // Selected on light bg: text stays dark (invert=true).
    const bool invertText = selected ? metrics.optionPopupSelectionLight : true;
    renderer.drawText(optionFontId, textX, textY, labelText, invertText, optionStyle);
  }
}
