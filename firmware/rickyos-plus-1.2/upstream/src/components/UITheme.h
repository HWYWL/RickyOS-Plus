#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/inx/InxTheme.h"

class CoverGridHomeUi;

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  enum class TextVerticalAlignment { TOP, CENTER, BOTTOM };

  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const;
  const BaseTheme& getTheme() const { return *currentTheme; }
  bool usesClassicTabs() const { return currentType == CrossPointSettings::UI_THEME::CLASSIC; }
  bool hasMainTabs() const { return currentType == CrossPointSettings::UI_THEME::INX; }
  bool showSelectionCursor() const;
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // Wraps only overflowing text, then aligns the complete line block within bounds.
  static void drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black = true,
                                      EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                      TextVerticalAlignment verticalAlignment = TextVerticalAlignment::CENTER);
  static bool supportsCoverGrid();
  static bool hasCoverGridHome();
  static void drawCoverGridHome(CoverGridHomeUi& home);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getStatusBarTextTopPadding(const GfxRenderer& renderer);
  static int getProgressBarHeight();

 private:
  // One lifetime-owned product theme: no heap churn or Classic fallback on reload.
  InxTheme fixedTheme;
  const ThemeMetrics* currentMetrics = &InxMetrics::values;
  BaseTheme* currentTheme = &fixedTheme;
  CrossPointSettings::UI_THEME currentType = CrossPointSettings::UI_THEME::INX;
  mutable ThemeMetrics adjustedMetrics;
  mutable bool metricsValid = false;
  mutable bool metricsForButtonHints = false;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
