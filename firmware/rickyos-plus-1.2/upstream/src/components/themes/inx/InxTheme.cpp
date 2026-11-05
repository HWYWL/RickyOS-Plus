#include "InxTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "InxItemLayout.h"
#include "components/RickyAaIcons.h"
#include "components/RickyDesignUi.h"
#include "components/RickyPageLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/inx_apps.h"
#include "components/icons/rickyNavigationIcons.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
// The product tab bar: 40 px icons drawn with ~2.5 px round strokes over small labels;
// the selected tab reverses its icon inside an ink capsule.
constexpr int kIconSize = 40;
constexpr int kPillWidth = ricky::ds::layout::kTabPillWidth;
constexpr int kPillHeight = ricky::ds::layout::kTabPillHeight;
constexpr int kUnderlineHeight = 5;
constexpr int kRowHeight = InxMenuGeometry::rowHeight;
constexpr int kRowPadding = UiHighDpiProfile::enabled ? UiHighDpiProfile::contentPadding : 20;
constexpr int kListIconSize = 24;
constexpr int kMenuIconSize = 32;
constexpr int kIconGap = 10;
constexpr int kMaxValueWidth = 200;
constexpr int kSideHintY = 345;
constexpr int kX3SideHintY = 155;
constexpr int kHintWidth = 80;
constexpr int kHintBarWidth = kHintWidth * 4 / 5;
constexpr int kHintBarInset = (kHintWidth - kHintBarWidth) / 2;
constexpr int kHintBarHeight = 5;
constexpr int kHintGap = 4;

void drawHintText(const GfxRenderer& renderer, const char* label, const int x, const int contentY,
                  const int contentBottom) {
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  const int textY = std::max(contentY, contentBottom - renderer.getLineHeight(SMALL_FONT_ID) + 1);
  const GfxRenderer::ClipScope clip(renderer, x + 1, contentY, kHintWidth - 2, contentBottom - contentY + 1);
  renderer.drawText(SMALL_FONT_ID, x + std::max(1, (kHintWidth - textWidth) / 2), textY, label);
}

const char* hintLabel(const char* label) {
  if (std::strcmp(label, tr(STR_DIR_LEFT)) == 0) return "<";
  if (std::strcmp(label, tr(STR_DIR_RIGHT)) == 0) return ">";
  return label;
}

const uint8_t* iconForTab(const MainTab tab) {
  // By tab, not by position: MainTabs::values sets the order.
  switch (tab) {
    case MainTab::Recent:
      return ricky_nav_home_40;
    case MainTab::Library:
      return ricky_nav_library_40;
    case MainTab::Apps:
      return ricky_nav_apps_40;
    case MainTab::StorageFiles:
      return ricky_nav_storage_40;
    case MainTab::Settings:
      return ricky_nav_settings_40;
    default:
      return nullptr;
  }
}

void drawInxIcon(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y) {
  if (RickyAaIcons::draw(renderer, icon, kIconSize, kIconSize, x, y, true)) return;
  constexpr int rowBytes = (kIconSize + 7) / 8;
  for (int row = 0; row < kIconSize; ++row) {
    for (int column = 0; column < kIconSize; ++column) {
      const uint8_t byte = icon[row * rowBytes + column / 8];
      if (((byte >> (7 - column % 8)) & 1U) == 0) renderer.drawPixel(x + column, y + row, true);
    }
  }
}

// Selected tab: an ink capsule with the icon knocked out in paper white. Pure
// 1-bit, so it stays crisp under the fast refresh the tab bar uses.
// `maxWidth` is the tab cell; a narrow screen shrinks the capsule, never below the icon.
void drawSelectedInxIcon(const GfxRenderer& renderer, const uint8_t* icon, const int iconX, const int iconY,
                         const int maxWidth) {
  const int pillWidth = std::max(kIconSize + 8, std::min(kPillWidth, maxWidth - 8));
  const int pillX = iconX + (kIconSize - pillWidth) / 2;
  const int pillY = iconY + (kIconSize - kPillHeight) / 2;
  renderer.fillRoundedRect(pillX, pillY, pillWidth, kPillHeight, kPillHeight / 2, Color::Black);
  if (RickyAaIcons::draw(renderer, icon, kIconSize, kIconSize, iconX, iconY, false)) return;
  constexpr int rowBytes = (kIconSize + 7) / 8;
  for (int row = 0; row < kIconSize; ++row) {
    for (int column = 0; column < kIconSize; ++column) {
      if ((icon[row * rowBytes + column / 8] & (0x80U >> (column % 8))) == 0)
        renderer.drawPixel(iconX + column, iconY + row, false);
    }
  }
}

void drawDottedSeparator(const GfxRenderer& renderer, const int x, const int y, const int width) {
  for (int px = x; px < x + width; px += 3) renderer.drawPixel(px, y, true);
}
}  // namespace

void InxTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title, const char* subtitle,
                          const bool backButton) const {
  // Product-wide hierarchical navigation: pushed touch pages need the same
  // visible Back target and title reserve as the shared FreeInkUI header.
  if (backButton && gpio.hasTouch()) {
    BaseTheme::drawHeader(renderer, rect, title, subtitle, true);
    return;
  }
  (void)backButton;
  constexpr int titleFont = UiHighDpiProfile::enabled ? UI_12_FONT_ID : NOTOSERIF_12_FONT_ID;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - InxMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, InxMetrics::values.batteryWidth, InxMetrics::values.batteryHeight},
                   showBatteryPercentage, UiHighDpiProfile::enabled ? SMALL_FONT_ID : STATUS_NUMERIC_FONT_ID);

  const int titleTop = rect.y + InxMetrics::values.batteryBarHeight;
  const int rightPadding = InxMetrics::values.contentSidePadding;
  int titleRight = rect.x + rect.width - rightPadding;
  if (subtitle && *subtitle) {
    const int subtitleWidth =
        std::min(renderer.getTextWidth(SMALL_FONT_ID, subtitle), std::max(0, rect.width / 2 - rightPadding));
    titleRight -= subtitleWidth + kIconGap;
    if (subtitleWidth > 0) {
      const int subtitleHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const Rect subtitleRect{titleRight + kIconGap,
                              titleTop + std::max(0, (renderer.getLineHeight(titleFont) - subtitleHeight) / 2),
                              subtitleWidth, subtitleHeight};
      const GfxRenderer::ClipScope clip(renderer, subtitleRect.x, subtitleRect.y, subtitleRect.width,
                                        subtitleRect.height);
      renderer.drawText(SMALL_FONT_ID, subtitleRect.x, subtitleRect.y, subtitle);
    }
  }

  if (title && *title && titleRight > rect.x + kRowPadding) {
    const GfxRenderer::ClipScope clip(renderer, rect.x + kRowPadding, titleTop, titleRight - rect.x - kRowPadding,
                                      renderer.getLineHeight(titleFont));
    renderer.drawText(titleFont, rect.x + kRowPadding, titleTop, title, true, EpdFontFamily::BOLD);
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void InxTheme::drawSubHeader(const GfxRenderer& renderer, const Rect rect, const char* label,
                             const char* rightLabel) const {
  if (UiHighDpiProfile::enabled) {
    const int pad = ricky::ds::space::kPageMargin;
    int right = rect.x + rect.width - pad;
    if (rightLabel && *rightLabel) {
      const int width = std::min(renderer.getTextWidth(RickyDesignUi::BODY, rightLabel), std::max(0, rect.width / 3));
      RickyDesignUi::text(renderer,
                          {static_cast<int16_t>(right - width), static_cast<int16_t>(rect.y),
                           static_cast<int16_t>(width), static_cast<int16_t>(rect.height)},
                          rightLabel, RickyDesignUi::CAPTION, false, freeink::ui::TextAlign::Right);
      right -= width + ricky::ds::space::kInlineGap;
    }
    RickyDesignUi::text(renderer,
                        {static_cast<int16_t>(rect.x + pad), static_cast<int16_t>(rect.y),
                         static_cast<int16_t>(std::max(0, right - rect.x - pad)), static_cast<int16_t>(rect.height)},
                        label, RickyDesignUi::HEADLINE);
    return;
  }
  const int padding = kRowPadding;
  int labelRight = rect.x + rect.width - padding;
  if (rightLabel && *rightLabel) {
    const int rightWidth =
        std::min(renderer.getTextWidth(UI_10_FONT_ID, rightLabel), std::max(0, rect.width / 2 - padding));
    labelRight -= rightWidth + kIconGap;
    if (rightWidth > 0) {
      const GfxRenderer::ClipScope clip(renderer, labelRight + kIconGap, rect.y, rightWidth, rect.height - 1);
      renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - padding - rightWidth,
                        rect.y + std::max(0, (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2), rightLabel);
    }
  }
  if (label && *label && labelRight > rect.x + padding) {
    const GfxRenderer::ClipScope clip(renderer, rect.x + padding, rect.y, labelRight - rect.x - padding,
                                      rect.height - 1);
    renderer.drawText(UI_10_FONT_ID, rect.x + padding,
                      rect.y + std::max(0, (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2), label, true,
                      EpdFontFamily::BOLD);
  }
  drawDottedSeparator(renderer, rect.x, rect.y + rect.height - 1, rect.width);
}

void InxTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                          const bool) const {
  if (tabs.empty()) return;
  const int count = static_cast<int>(tabs.size());
  for (int index = 0; index < count; ++index) {
    const int left = rect.x + rect.width * index / count;
    const int right = rect.x + rect.width * (index + 1) / count;
    const bool active = tabs[index].selected;
    if (active) renderer.fillRect(left, rect.y, right - left, rect.height - 1, true);
    const int textWidth =
        renderer.getTextWidth(UI_10_FONT_ID, tabs[index].label, active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int textX = left + std::max(0, (right - left - textWidth) / 2);
    const GfxRenderer::ClipScope clip(renderer, left, rect.y, right - left, rect.height - 1);
    renderer.drawText(UI_10_FONT_ID, textX,
                      rect.y + std::max(0, (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2),
                      tabs[index].label, !active, active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

bool InxTheme::tabIndexFromPoint(const GfxRenderer&, const Rect rect, const std::vector<TabInfo>& tabs, const int x,
                                 const int y, int& index) const {
  if (tabs.empty() || x < rect.x || x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height) return false;
  index = (x - rect.x) * static_cast<int>(tabs.size()) / rect.width;
  return true;
}

void InxTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const {
  if (!buttonHintsVisible()) return;

  const GfxRenderer::Orientation original = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonHeight = InxMetrics::values.buttonHintsHeight;
  const int pageWidth = renderer.getScreenWidth();
  const int margin = gpio.deviceIsX3() ? 65 : 58;
  const int gap = gpio.deviceIsX3() ? 12 : 8;
  const int positions[] = {margin, margin + kHintWidth + gap, pageWidth - margin - kHintWidth * 2 - gap,
                           pageWidth - margin - kHintWidth};
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int hintY = pageHeight - buttonHeight;
  const int barY = pageHeight - kHintBarHeight;
  const int contentBottom = barY - kHintGap - 1;

  for (int i = 0; i < 4; ++i) {
    renderer.fillRect(positions[i], hintY, kHintWidth, buttonHeight, false);
    if (!labels[i] || !*labels[i]) continue;
    renderer.fillRectDither(positions[i] + kHintBarInset, barY, kHintBarWidth, kHintBarHeight, Color::DarkGray);
    drawHintText(renderer, hintLabel(labels[i]), positions[i], hintY, contentBottom);
  }
  renderer.setOrientation(original);
}

void InxTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) return;
  constexpr int width = InxMetrics::values.sideButtonHintsWidth;
  constexpr int height = 78;
  const int screenWidth = renderer.getScreenWidth();
  const char* labels[] = {topBtn, bottomBtn};
  const int xs[] = {gpio.deviceIsX3() ? 0 : screenWidth - width, screenWidth - width};
  const int firstY = gpio.deviceIsX3() ? kX3SideHintY : kSideHintY;
  const int ys[] = {firstY, gpio.deviceIsX3() ? firstY : firstY + height + 5};

  for (int i = 0; i < 2; ++i) {
    if (!labels[i] || !*labels[i]) continue;
    renderer.fillRect(xs[i], ys[i], width, height, false);
    renderer.drawRect(xs[i], ys[i], width, height, true);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
    renderer.drawTextRotated90CW(SMALL_FONT_ID, xs[i], ys[i] + (height + textWidth) / 2, labels[i]);
  }
}

int InxTheme::getListRowStep(const bool hasSubtitle) const {
  return UiHighDpiProfile::enabled && hasSubtitle ? UiHighDpiProfile::subtitleRowHeight : kRowHeight;
}

int InxTheme::getListPageItems(const int contentHeight, const bool hasSubtitle) const {
  return std::max(1, contentHeight / getListRowStep(hasSubtitle));
}

void InxTheme::drawList(const GfxRenderer& renderer, const Rect rect, const int itemCount, const int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle,
                        const std::function<UIIcon(int index)>& rowIcon,
                        const std::function<std::string(int index)>& rowValue, const bool,
                        const std::function<bool(int index)>& rowDimmed, const bool showSelection,
                        const std::function<bool(int index)>& rowHeading) const {
  if (itemCount <= 0 || rect.height <= 0) return;

  const int rowHeight = getListRowStep(rowSubtitle != nullptr);
  const int pageItems = getListPageItems(rect.height, rowSubtitle != nullptr);
  const int pageStart = selectedIndex >= 0 ? selectedIndex / pageItems * pageItems : 0;
  const int pageEnd = std::min(itemCount, pageStart + pageItems);
  const bool hasScrollBar = itemCount > pageItems;
  const int contentRight = rect.x + rect.width - (hasScrollBar ? 10 : 0);
  const int iconSize = rowSubtitle != nullptr ? kMenuIconSize : kListIconSize;
  const bool selectionVisible = showSelection && UITheme::getInstance().showSelectionCursor();

  for (int index = pageStart; index < pageEnd; ++index) {
    const int slot = index - pageStart;
    const int rowY = rect.y + slot * rowHeight;
    const bool selected = selectionVisible && index == selectedIndex;
    if (selected) renderer.fillRect(rect.x, rowY, rect.width, rowHeight, true);

    int textX = rect.x + kRowPadding;
    if (rowIcon) {
      if (UiHighDpiProfile::enabled) {
        const auto bitmap = listIconFor(rowIcon(index), UiHighDpiProfile::controlIconSize);
        if (bitmap) {
          freeink::ui::GfxRendererTarget target(renderer, true);
          target.bitmap(
              freeink::ui::Rect{static_cast<int16_t>(textX), static_cast<int16_t>(rowY + (rowHeight - 48) / 2), 48, 48},
              bitmap, freeink::ui::BitmapMode::Center,
              freeink::ui::Paint::solid(selected ? freeink::ui::Color::White : freeink::ui::Color::Black));
          textX += 48 + kIconGap;
        }
      } else if (const uint8_t* bitmap = iconForName(rowIcon(index), iconSize)) {
        const int iconY = rowY + (rowHeight - iconSize) / 2;
        if (selected)
          renderer.drawIconInverted(bitmap, textX, iconY, iconSize);
        else
          renderer.drawIcon(bitmap, textX, iconY, iconSize);
        textX += iconSize + kIconGap;
      }
    }

    std::string value;
    int valueWidth = 0;
    if (rowValue) {
      value = renderer.truncatedText(UI_10_FONT_ID, rowValue(index).c_str(), kMaxValueWidth);
      valueWidth = value.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, value.c_str()) + kIconGap;
    }
    const bool heading = rowHeading && rowHeading(index);
    const int titleFont = heading ? UI_12_FONT_ID : UI_10_FONT_ID;
    const auto titleStyle = heading ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = std::max(1, contentRight - kRowPadding - textX - valueWidth);
    const std::string title = renderer.truncatedText(titleFont, rowTitle(index).c_str(), textWidth, titleStyle);
    const int titleY = rowY + (rowSubtitle ? (UiHighDpiProfile::enabled ? 20 : 12)
                                           : (rowHeight - renderer.getLineHeight(titleFont)) / 2);
    renderer.drawText(titleFont, textX, titleY, title.c_str(), !selected, titleStyle);

    if (rowSubtitle) {
      const std::string subtitle = renderer.truncatedText(SMALL_FONT_ID, rowSubtitle(index).c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, textX,
                        rowY + (UiHighDpiProfile::enabled ? 20 + renderer.getLineHeight(titleFont) + 6 : 36),
                        subtitle.c_str(), !selected);
    }
    if (!value.empty()) {
      const int valueX = contentRight - kRowPadding - renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
      const int valueY = rowY + (rowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, valueX, valueY, value.c_str(), !selected);
    }
    if (rowDimmed && rowDimmed(index) && !selected) {
      drawDitherMask(renderer, textX, rowY, std::max(0, contentRight - textX), rowHeight - 1);
    }
    drawDottedSeparator(renderer, rect.x, rowY + rowHeight - 1, rect.width);
  }

  drawSideScrollBar(renderer, rect, itemCount, pageStart, pageItems);
}

void InxTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount, const int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon, const int) const {
  const int pageItems = InxMenuGeometry::pageItems(rect.height);
  const int pageStart = InxMenuGeometry::pageStart(selectedIndex, buttonCount, rect.height);
  const int pageEnd = std::min(buttonCount, pageStart + pageItems);
  const bool selectionVisible = UITheme::getInstance().showSelectionCursor();
  for (int index = pageStart; index < pageEnd; ++index) {
    const int rowY = rect.y + (index - pageStart) * kRowHeight;
    const bool selected = selectionVisible && index == selectedIndex;
    if (selected) renderer.fillRect(rect.x, rowY, rect.width, kRowHeight, true);

    int textX = rect.x + kRowPadding;
    if (rowIcon) {
      const UIIcon icon = rowIcon(index);
      if (InxAppIcons::get(icon)) {
        const int iconY = rowY + (kRowHeight - kMenuIconSize) / 2;
        InxAppIcons::draw(renderer, icon, textX, iconY, 1, selected);
        textX += kMenuIconSize + kIconGap;
      } else if (const uint8_t* bitmap = iconForName(icon, kMenuIconSize)) {
        const int iconY = rowY + (kRowHeight - kMenuIconSize) / 2;
        if (selected)
          renderer.drawIconInverted(bitmap, textX, iconY, kMenuIconSize);
        else
          renderer.drawIcon(bitmap, textX, iconY, kMenuIconSize);
        textX += kMenuIconSize + kIconGap;
      }
    }

    const std::string label = renderer.truncatedText(UI_12_FONT_ID, buttonLabel(index).c_str(),
                                                     std::max(1, rect.x + rect.width - kRowPadding - textX));
    const int textY = rowY + (kRowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, label.c_str(), !selected);
    drawDottedSeparator(renderer, rect.x, rowY + kRowHeight - 1, rect.width);
  }
  drawSideScrollBar(renderer, rect, buttonCount, pageStart, pageItems);
}

InxTheme::MenuRowGeometry InxTheme::getMenuRowGeometry(const GfxRenderer&, const Rect& rect, const int selectedIndex,
                                                       const int rowCount) const {
  // Mirror of InxTheme::drawButtonMenu: rows start at rect.y, step by the
  // fixed row height and are paged.
  const int pageItems = InxMenuGeometry::pageItems(rect.height);
  const int pageStart = InxMenuGeometry::pageStart(selectedIndex, rowCount, rect.height);
  return {rect.y,
          kRowHeight,
          kRowHeight,
          pageStart,
          std::min(rowCount - pageStart, pageItems),
          rect.x + kRowPadding,
          rect.x + rect.width};
}

void InxTheme::drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                               const int selectedIndex, const char* message) const {
  if (options.empty()) return;

  const int optionCount = static_cast<int>(options.size());
  const int selected = std::clamp(selectedIndex, 0, optionCount - 1);
  const bool hasMessage = message && message[0];
  const auto layout = InxOptionGeometry::layout(UITheme::getInstance().getScreenSafeArea(renderer, true, false),
                                                optionCount, selected, hasMessage);
  const int visibleRows = layout.rows;
  const int maxStart = optionCount - visibleRows;
  const int start = layout.first;
  const int panelWidth = layout.panel.width;
  const int panelHeight = layout.panel.height;
  const int panelX = layout.panel.x;
  const int panelY = layout.panel.y;
  const bool selectionVisible = UITheme::getInstance().showSelectionCursor();

  renderer.fillRoundedRect(panelX, panelY, panelWidth, panelHeight, ricky::ds::radius::kContainer, Color::White);
  const bool hasTitle = title && title[0];
  if (hasTitle) {
    RickyDesignUi::text(renderer,
                        {static_cast<int16_t>(panelX + 32), static_cast<int16_t>(panelY),
                         static_cast<int16_t>(panelWidth - 64), static_cast<int16_t>(InxOptionGeometry::headerHeight)},
                        title, RickyDesignUi::HEADLINE);
  }
  if (hasMessage) {
    // Message-only dialogs (no title) start the body at the top of the header.
    const int top = panelY + (hasTitle ? InxOptionGeometry::headerHeight : InxOptionGeometry::headerHeight / 3);
    const int bottom = panelY + layout.header;
    RickyDesignUi::text(renderer,
                        {static_cast<int16_t>(panelX + 32), static_cast<int16_t>(top),
                         static_cast<int16_t>(panelWidth - 64), static_cast<int16_t>(bottom - top)},
                        message, RickyDesignUi::BODY, false, freeink::ui::TextAlign::Left, 4);
  }

  for (int slot = 0; slot < visibleRows; ++slot) {
    const int optionIndex = start + slot;
    const int rowY = panelY + layout.header + slot * InxOptionGeometry::rowHeight;
    const bool isSelected = selectionVisible && optionIndex == selected;
    if (isSelected) {
      const Rect row = layout.optionRect(slot);
      renderer.fillRoundedRect(row.x, row.y, row.width, row.height, ricky::ds::radius::kControl, Color::Black);
    }
    RickyDesignUi::text(renderer,
                        {static_cast<int16_t>(panelX + 40), static_cast<int16_t>(rowY),
                         static_cast<int16_t>(panelWidth - 80), static_cast<int16_t>(InxOptionGeometry::rowHeight)},
                        options[optionIndex].c_str(), RickyDesignUi::BODY, isSelected);
  }

  if (optionCount > visibleRows) {
    const int trackX = panelX + panelWidth - 10;
    const int trackY = panelY + layout.header;
    const int trackHeight = visibleRows * InxOptionGeometry::rowHeight;
    const int thumbHeight = std::max(8, trackHeight * visibleRows / optionCount);
    const int thumbY = trackY + start * (trackHeight - thumbHeight) / maxStart;
    renderer.fillRect(trackX, trackY, 2, trackHeight, true);
    renderer.fillRect(trackX - 2, thumbY, 6, thumbHeight, true);
  }

  renderer.drawRoundedRect(panelX, panelY, panelWidth, panelHeight, ricky::ds::stroke::kEmphasis,
                           ricky::ds::radius::kContainer, true);
}

void InxTheme::drawMainTabBar(const GfxRenderer& renderer, const Rect rect, const MainTab selected) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const bool tabsAtBottom = SETTINGS.inxTabPosition == CrossPointSettings::INX_TAB_BOTTOM;
  const int separatorY = tabsAtBottom ? rect.y : rect.y + rect.height - 1;
  // A light rule, not an ink line: the bar sits under the page without boxing it in
  // (ink on a B/W frame, the 1-bit fallback for the line gray).
  if (renderer.isGrayscale16Active())
    renderer.fillGrayscale16Rect(rect.x, separatorY, rect.width, 1, ricky::ds::gray::kLine);
  else
    renderer.fillRect(rect.x, separatorY, rect.width, 1, true);
  // Rows are laid out for the capsule so it never touches the separator or labels.
  auto rows = RickyPageLayout::navigationRows(rect, kPillHeight, renderer.getLineHeight(SMALL_FONT_ID));
  rows.iconY += (kPillHeight - kIconSize) / 2;
  const auto labelFor = [](const MainTab tab) {
    switch (tab) {
      case MainTab::Recent:
        return StrId::STR_RICKY_NAV_HOME;
      case MainTab::Library:
        return StrId::STR_LIBRARY;
      case MainTab::Apps:
        return StrId::STR_APPS_TITLE;
      case MainTab::StorageFiles:
        return StrId::STR_RICKY_STORAGE;
      default:
        return StrId::STR_SETTINGS_TITLE;
    }
  };
  for (size_t i = 0; i < MainTabs::values.size(); ++i) {
    const MainTab tab = MainTabs::values[i];
    const auto bounds = MainTabs::tabBounds(static_cast<int>(i), rect.width);
    const int left = rect.x + bounds.left;
    const int width = bounds.right - bounds.left;
    const GfxRenderer::ClipScope clip(renderer, left, rect.y, width, rect.height);
    const int iconX = left + (width - kIconSize) / 2;
    if (const uint8_t* icon = iconForTab(tab)) drawInxIcon(renderer, icon, iconX, rows.iconY);
    // The current tab: a short ink bar on the bar's edge above it and an ink label; the
    // others keep ink icons over secondary labels. No capsule.
    if (tab == selected) {
      constexpr int kMarkWidth = 48, kMarkHeight = 4;
      const int markY = tabsAtBottom ? rect.y : rect.y + rect.height - kMarkHeight;
      renderer.fillRect(left + (width - kMarkWidth) / 2, markY, kMarkWidth, kMarkHeight, true);
    }
    const char* label = I18N.get(labelFor(tab));
    const int textX = left + std::max(0, (width - renderer.getTextWidth(SMALL_FONT_ID, label)) / 2);
    const GfxRenderer::TextInkScope ink(renderer, tab == selected ? ricky::ds::gray::kInk : ricky::ds::gray::kInk2);
    renderer.drawText(SMALL_FONT_ID, textX, rows.labelY, label, true);
  }
}

void InxTheme::drawMainTabStatusBar(const GfxRenderer& renderer, const Rect rect) const {
  if (rect.width <= 0 || rect.height <= 0) return;
  constexpr int numericFontId = UiHighDpiProfile::enabled ? SMALL_FONT_ID : STATUS_NUMERIC_FONT_ID;
  constexpr int edgeInset = ricky::ds::space::kPageMargin;
  constexpr int batteryTextOffset = 6;  // drawBatteryRight offsets the icon below the text origin.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool tabsAtBottom = SETTINGS.inxTabPosition == CrossPointSettings::INX_TAB_BOTTOM;
  // The battery cap and numeric glyphs include the positioning edge pixel.
  const int right = std::min(rect.x + rect.width - 1, renderer.getScreenWidth() - edgeInset);
  const int textY = UiHighDpiProfile::enabled ? rect.y + (rect.height - renderer.getLineHeight(numericFontId)) / 2
                    : tabsAtBottom            ? std::max(rect.y, edgeInset) - batteryTextOffset
                                   : std::min(rect.y + rect.height - 1, renderer.getScreenHeight() - edgeInset) -
                                         metrics.batteryHeight - batteryTextOffset;
  const GfxRenderer::ClipScope clip(renderer, rect.x, rect.y, rect.width, rect.height);
  if (tabsAtBottom) {
    char time[9] = "--:--";
    TimeUtils::formatCurrentTime(time, sizeof(time), SETTINGS.clockFormat == 1);
    renderer.drawText(numericFontId, std::max(rect.x, edgeInset), textY, time);
  }
  drawBatteryRight(renderer, Rect{right - metrics.batteryWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                   SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS,
                   numericFontId);
}
