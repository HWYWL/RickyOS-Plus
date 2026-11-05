#include "UITheme.h"

#include <EpdFont.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/notosans_18_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "components/SelectionCursorPolicy.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/inx/InxTheme.h"

// The registered families keep these stable addresses across theme changes.
EpdFont ui10RegularFont(&notosans_14_regular);
EpdFont ui10BoldFont(&notosans_14_bold);
EpdFont ui12RegularFont(&notosans_16_regular);
EpdFont ui12BoldFont(&notosans_16_bold);

extern EpdFont offlineReaderFont;
EpdFont ui18RegularFont(&notosans_16_regular);
EpdFont ui18BoldFont(&notosans_16_bold);
EpdFontFamily control18FontFamily(&ui12RegularFont, &ui12BoldFont);

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  SETTINGS.enforceProductLayout();
  const bool inx = SETTINGS.uiTheme == CrossPointSettings::INX;
  (void)inx;
  ui18RegularFont.data = &notosans_16_regular;
  ui18BoldFont.data = &notosans_16_bold;
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

bool UITheme::supportsCoverGrid() { return false; }

bool UITheme::hasCoverGridHome() { return false; }

void UITheme::drawCoverGridHome(CoverGridHomeUi& home) { (void)home; }

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  (void)type;
  SETTINGS.enforceProductLayout();
  currentTheme = &fixedTheme;
  currentMetrics = &InxMetrics::values;
  currentType = CrossPointSettings::INX;
  metricsValid = false;
}

const ThemeMetrics& UITheme::getMetrics() const {
  // Touch availability can flip after static construction, and the setting can
  // change while this screen is open, so cache against the effective policy.
  const bool showButtonHints = currentTheme->buttonHintsVisible();
  if (!metricsValid || showButtonHints != metricsForButtonHints) {
    adjustedMetrics = *currentMetrics;
    // InxMetrics already applies the hardware preset, then the product's
    // design tokens. Applying the old preset again silently undid margins,
    // subpage header height, keyboard size and row cadence at runtime.
    if (!showButtonHints) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    metricsForButtonHints = showButtonHints;
    metricsValid = true;
  }
  return adjustedMetrics;
}

bool UITheme::showSelectionCursor() const {
#ifdef CROSSPOINT_EMULATED
  return true;
#else
  return SelectionCursorPolicy::visible(currentType == CrossPointSettings::UI_THEME::INX, gpio.hasTouch(),
                                        gpio.lastInputModality());
#endif
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight =
      UITheme::getInstance().getScreenSafeArea(renderer).height - reservedHeight - extraReservedHeight;
  return UITheme::getInstance().getTheme().getListPageItems(availableHeight, hasSubtitle);
}

// Screen area excluding the bezel and button hints.
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  safeArea = Rect{left, top, screenWidth - left - right, screenHeight - top - bottom};
#endif
  const ThemeMetrics metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasImageExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();

  // Layout reservation is hardware-agnostic: pass clockAvailable=true so the
  // reserved height does not depend on whether an RTC is present.
  return (sb.textLaneVisible() ? (metrics.statusBarVerticalMargin) : 0) +
         (sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  return sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0;
}

int UITheme::getStatusBarTextTopPadding(const GfxRenderer& renderer) {
  if (!UiHighDpiProfile::enabled) return 0;
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  constexpr int textFontId = READER_STATUS_FONT_ID;
#else
  constexpr int textFontId = SMALL_FONT_ID;
#endif
  const int lineHeight =
      std::max(renderer.getLineHeight(textFontId), renderer.getLineHeight(BaseTheme::STATUS_NUMERIC_FONT_ID));
  return std::max(0, UITheme::getInstance().getMetrics().statusBarVerticalMargin - lineHeight -
                         UiHighDpiProfile::readerStatusBottomPadding);
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
  int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
  for (const auto& line : lines) {
    drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
    y += lineHeight;
  }
}
