#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include "../hal/DisplayRefreshContext.h"

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;
class TtfEpdFont;

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

namespace glyphBitmap {
struct Frame;
}

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

// Paint-time breakdown for device measurement: -DRICKYOS_PAINT_PROFILE=1 times the
// outermost fill / text / shape / image call of each kind; off in shipped firmware.
#ifndef RICKYOS_PAINT_PROFILE
#define RICKYOS_PAINT_PROFILE 0
#endif

class GfxRenderer {
 public:
  // Glyph = glyph bitmap fetches (font cache / decompressor / SD), counted inside Text too.
  enum class PaintKind : uint8_t { Fill, Text, Shape, Image, Glyph, Count };
  struct PaintProfile {
    uint32_t us[static_cast<int>(PaintKind::Count)] = {};
    uint32_t calls[static_cast<int>(PaintKind::Count)] = {};
  };
#if RICKYOS_PAINT_PROFILE
  static PaintProfile& paintProfile();
  // Times only the outermost profiled call, so nested fills inside a rounded fill or
  // text inside a centered draw are not counted twice.
  class PaintScope {
   public:
    explicit PaintScope(PaintKind kind);
    ~PaintScope();
    PaintScope(const PaintScope&) = delete;
    PaintScope& operator=(const PaintScope&) = delete;

   private:
    PaintKind kind_;
    uint32_t start_ = 0;
    bool outer_ = false;
  };
#endif
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  struct TwoBitPixel {
    bool draw;
    bool state;
  };

  // Map 0=black, 1=dark gray, 2=light gray, 3=white into the active
  // framebuffer plane. Keep every 2-bit renderer on this single table.
  static constexpr TwoBitPixel mapTwoBitPixel(const RenderMode mode, const uint8_t value) {
    if (mode == BW) return {value < 3, true};
    if (mode == GRAYSCALE_MSB) return {value == 1 || value == 2, false};
    return {value == 1, false};
  }

  static constexpr TwoBitPixel mapTwoBitGlyphCoverage(const RenderMode mode, const uint8_t coverage) {
    // Font coverage is 0=white..3=black; bitmap pixels use 0=black..3=white.
    return mapTwoBitPixel(mode, 3 - coverage);
  }

  static constexpr bool framebufferState(const RenderMode mode, const bool state) {
    (void)mode;
    return state;
  }

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  void mapGrayRefreshArea(int& x, int& y, int& width, int& height) const;
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory
  static constexpr uint8_t MAX_SYNTHETIC_BOLD_PIXELS = 3;

  HalDisplay& display;
  mutable uint8_t* grayscale16Buffer = nullptr;
  // While capturing, a screen is redrawn into the 16-level frame for a later commit:
  // display pushes are skipped and clearScreen() also clears that frame.
  bool displayCapture_ = false;
  RenderMode renderMode;
  mutable bool absoluteGrayPlanes = false;
  Orientation orientation;
  bool fadingFix;
  mutable uint8_t syntheticBoldPixels = 0;
  mutable const uint8_t* textCoverageCurve_ = nullptr;  // 16 entries; null = coverage as drawn
  mutable uint8_t textSpread_ = 0;                      // 0 none, 1 half a pixel, 2 a pixel (TextWeightScope)
  mutable uint8_t textInkLevel_ = 0;                    // native UI text tone, scoped per run
  void writeGrayscale16Physical(int px, int py, uint8_t level) const;
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  // One-shot refresh-mode override consumed by the next displayBuffer() call.
  // Lets an activity force e.g. a HALF_REFRESH for a single frame (used by the
  // reading-stats screens) without threading the mode through every render path.
  mutable bool nextRefreshOverridePending = false;
  mutable HalDisplay::RefreshMode nextRefreshOverride = HalDisplay::FAST_REFRESH;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;
  mutable std::map<int, uint16_t> sdCardFontScales_;  // fontId -> 8.8 fixed point scale (256=1.0x)
  // TTF (vector) fonts: rebuilt per page by ensureSdCardFontReady(). Mutable for
  // the same reason as sdCardFonts_ (const layout path triggers a rebuild).
  mutable std::map<int, TtfEpdFont*> ttfFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;
  mutable int clipLeft_ = 0;
  mutable int clipTop_ = 0;
  mutable int clipRight_ = 32767;
  mutable int clipBottom_ = 32767;

  // Ordered UI fallbacks: optional SD face, then the built-in CJK subset.
  // Resolve the whole string through one face for consistent draw/measure metrics.
  std::map<int, std::array<int, 2>> fallbackFontMap_;
  // fontId -> the family it should actually resolve to (see setPreferredFont()).
  std::map<int, int> preferredFontMap_;
  // The family a font id really resolves to (identity unless rebound). EVERY path that
  // looks a family up -- glyph coverage, metrics, preloading -- must go through this,
  // or a rebound id is laid out with the built-in face and drawn in the SD one.
  int resolveFontFamilyId(int fontId) const;

  // Resolve a missing-glyph fallback; the high-density profile checks whole-run coverage.
  int resolveTextFontId(int fontId, const char* text, EpdFontFamily::Style style) const;
  void ensureSdGlyphsResident(int fontId, const char* text, EpdFontFamily::Style style, bool metadataOnly) const;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() {
    cancelGrayscale16();
    freeBwBufferChunks();
  }
  GfxRenderer(const GfxRenderer&) = delete;
  GfxRenderer& operator=(const GfxRenderer&) = delete;
  GfxRenderer(GfxRenderer&&) = delete;
  GfxRenderer& operator=(GfxRenderer&&) = delete;

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears the font, its SD registration, and only fallback mappings that
  // reference fontId. Coupled to avoid dangling font pointers/ids when callers
  // free the underlying font and forget the renderer-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
    sdCardFontScales_.erase(fontId);
    std::erase_if(preferredFontMap_,
                  [fontId](const auto& mapping) { return mapping.first == fontId || mapping.second == fontId; });
    for (auto& [primary, fallbacks] : fallbackFontMap_) {
      std::replace(fallbacks.begin(), fallbacks.end(), fontId, 0);
    }
    std::erase_if(fallbackFontMap_, [fontId](const auto& mapping) {
      return mapping.first == fontId || (mapping.second[0] == 0 && mapping.second[1] == 0);
    });
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);
  void prewarmFallbackText(int fontId, TextGetter getter, const void* ctx, uint32_t textCount,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void prewarmFallbackText(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() {
    sdCardFonts_.clear();
    sdCardFontScales_.clear();
  }
  void registerSdCardFontScale(int fontId, uint16_t scale) { sdCardFontScales_[fontId] = scale; }
  void clearSdCardFontScales() { sdCardFontScales_.clear(); }
  uint16_t getSdCardFontScale(int fontId) const {
    auto it = sdCardFontScales_.find(fontId);
    return (it != sdCardFontScales_.end()) ? it->second : 256;
  }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // TTF (vector) fonts rendered via TtfEpdFont/FreeInkFont. Registered like an
  // ordinary EpdFontFamily (insertFont), plus tracked here so ensureSdCardFontReady()
  // rebuilds their per-page glyph set on demand — the eager analogue of the SD
  // font prewarm. The TtfEpdFont is owned by the caller (SdCardFontSystem).
  void registerTtfFont(int fontId, TtfEpdFont* font) { ttfFonts_[fontId] = font; }
  void unregisterTtfFont(int fontId) { ttfFonts_.erase(fontId); }
  const std::map<int, TtfEpdFont*>& getTtfFonts() const { return ttfFonts_; }
  // Register size-matched UI fallbacks; the built-in backup survives SD unload.
  void setFallbackFont(int primaryFontId, int fallbackFontId, int backupFontId = 0) {
    fallbackFontMap_[primaryFontId] = {fallbackFontId, backupFontId};
  }
  void clearFallbackFonts() { fallbackFontMap_.clear(); }
  // Rebind which family a font id actually resolves to, leaving fontMap untouched.
  // Read Pico sets this so its whole UI renders in the selected reading family at the
  // UI's own point sizes; without it only the handful of screens that go through
  // uiScaleSpec() get those sizes, and the ~70 that pass UI_10_FONT_ID / UI_12_FONT_ID
  // straight to the renderer keep the built-in faces.
  //
  // Resolving here -- rather than swapping fontMap entries, which insertFont() refuses
  // -- keeps measuring and drawing on one face, because every text and metric path goes
  // through resolveFontFamilyId(). The built-in family stays reachable via this id's
  // fallback entry, so glyphs the SD face lacks still render. Empty by default, so
  // every other target behaves exactly as before.
  void setPreferredFont(int fontId, int preferredFontId) { preferredFontMap_[fontId] = preferredFontId; }
  void clearPreferredFonts() { preferredFontMap_.clear(); }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  // Packed variant for the paragraph layout path: each segment holds
  // consecutive NUL-terminated words (WordStore chunks), so a whole paragraph
  // is scanned without materializing per-word strings.
  void ensureSdCardFontReady(int fontId, const char* const* segments, const size_t* segmentLens, size_t segmentCount,
                             bool includeSpace, bool includeHyphen, uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }
  void setNextMainTabTurn(int x, int y, int width, int height, bool fromRight) const;

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH,
                     DisplayRefreshContext context = DisplayRefreshContext::Normal) const;
  // Force the next displayBuffer() to use `mode`, overriding its argument once.
  void requestNextRefresh(const HalDisplay::RefreshMode mode) const {
    nextRefreshOverride = mode;
    nextRefreshOverridePending = true;
  }
  void clearNextRefreshOverride() const { nextRefreshOverridePending = false; }
  // Direct-gray UI must honor wake/overlay cleanup requests without consuming
  // them before its transaction succeeds; a failed commit still needs B/W cleanup.
  bool nextRefreshNeedsCleanup() const {
    return nextRefreshOverridePending && nextRefreshOverride != HalDisplay::FAST_REFRESH;
  }
  void requestNextFullRefresh() const { requestNextRefresh(HalDisplay::FULL_REFRESH); }
  // One-shot: the next displayBuffer()/displayBufferAsync() call uses `mode`
  // instead of what its caller asked for, then the override clears itself.
  // Lets a closing overlay (the control center's refresh tile) hand a
  // ghost-cleanup waveform to the repaint of whatever screen is underneath,
  // which it cannot reach directly.
  void promoteNextRefresh(const HalDisplay::RefreshMode mode) const { requestNextRefresh(mode); }
  // Non-blocking refresh: starts the waveform and returns so CPU work (e.g.
  // grayscale strip rendering) can overlap the panel's refresh time. The
  // framebuffer must stay untouched until waitRefreshComplete(). Falls back to
  // a blocking refresh when fadingFix is enabled or the panel lacks deferral
  // support. See HalDisplay::displayBufferAsync for the baseline contract.
  void displayBufferAsync(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH,
                          DisplayRefreshContext context = DisplayRefreshContext::Normal) const;
  void waitRefreshComplete() const;
  // True when displayBufferAsync() genuinely overlaps: panel defers and
  // fadingFix isn't forcing the blocking path. Callers can skip overlap
  // scaffolding (e.g. whole-plane grayscale buffers) when false.
  bool supportsAsyncRefresh() const;
  // True while the display renders inverted (night mode). Grayscale display
  // paths are deliberately disabled while inverted, so callers that defer the
  // B/W base to a gray pass must fall back to a per-page B/W display.
  // Non-inline: the host-test HalDisplay (crosspoint-simulator) lacks
  // isInverted(), and the header is compiled by test targets.
  bool isInverted() const;
  // True when the display can overlap an ordinary B/W refresh with grayscale
  // composition without bypassing a required grayscale base waveform.
  HalDisplay::GrayscaleCapabilities grayscaleCapabilities(
      HalDisplay::GrayscaleMode mode = HalDisplay::GrayscaleMode::Overlay) const;
  // Compatibility queries for Overlay mode.
  bool supportsAsyncGrayscaleBase() const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Logical-coordinate clip rectangle (scissor). While set, drawPixel() drops
  // pixels outside [x, x+width) x [y, y+height) without logging. Coordinates are
  // logical (pre-rotation), matching what activities pass to draw calls. Prefer
  // ClipScope below; bare setters exist for symmetry with the strip API.
  void clearClipRect() const { setClipRect(0, 0, 32767, 32767); }

  // RAII guard for setClipRect()/clearClipRect(). Restores the clip on scope
  // exit (including early returns), so a forgotten clear can never silently
  // clip a later frame or activity. Non-nesting: it clears on destruction
  // rather than restoring a previous rect (sufficient for the flat use here).
  struct ClipScope {
    const GfxRenderer& renderer;
    const std::array<int, 4> previous;
    ClipScope(const GfxRenderer& r, const int x, const int y, const int width, const int height)
        : renderer(r), previous(r.getClipRect()) {
      renderer.setClipRect(x, y, width, height);
    }
    ~ClipScope() { renderer.setClipRect(previous[0], previous[1], previous[2], previous[3]); }
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;
  };

  // Drawing
  // UI drawing clip in logical coordinates; independent of panel orientation.
  std::array<int, 4> getClipRect() const {
    return {clipLeft_, clipTop_, clipRight_ - clipLeft_, clipBottom_ - clipTop_};
  }
  void setClipRect(int x, int y, int width, int height) const {
    clipLeft_ = x;
    clipTop_ = y;
    clipRight_ = x + width;
    clipBottom_ = y + height;
  }
  void drawPixel(int x, int y, bool state = true) const;
  // Draw glyph ink with clipping and orientation resolved once per glyph.
  void drawGlyphBitmap(const uint8_t* bitmap, int width, int height, const glyphBitmap::Frame& frame, bool twoBit,
                       RenderMode mode, bool state) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  void drawIconInverted(const uint8_t bitmap[], int x, int y, int size) const;
  bool drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0, float cropY = 0,
                  bool preserveTransparency = false) const;
  bool drawBitmapCropToFill(const Bitmap& bitmap, int x, int y, int width, int height) const;
  bool drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void preserveImagePolarity(int x, int y, int width, int height) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Snapshot / restore a screen-coordinate framebuffer region (byte-aligned in
  // panel memory). readFramebufferRegion returns the bytes written to dst, or
  // 0 when the region is empty, offscreen, or exceeds dstCapacity. Pass the
  // same rectangle to writeFramebufferRegion to restore the saved pixels.
  // Enables partial-repaint patterns (e.g. moving a selection highlight)
  // without re-rendering the whole page.
  size_t readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const;
  void writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src);

  // Text
  // Page-local guard for synthetic bold. Restores the previous renderer state
  // so EPUB content cannot leak the effect into status bars or other UI.
  // Text stroke weight: while in scope, every anti-aliased glyph pixel's 0..15 coverage
  // goes through `curve` (16 entries, 0 -> 0, 15 -> 15). Lifting the edge coverage reads
  // as a heavier stroke, lowering it as a lighter one. `spread` grows every stroke right
  // and down by half a pixel (1) or a pixel (2) in coverage space, for weights past what
  // the edges alone can give. Layout is untouched either way.
  class TextWeightScope {
   public:
    TextWeightScope(const GfxRenderer& renderer, const uint8_t* curve, const uint8_t spread = 0)
        : renderer_(renderer), previousCurve_(renderer.textCoverageCurve_), previousSpread_(renderer.textSpread_) {
      renderer_.textCoverageCurve_ = curve;
      renderer_.textSpread_ = spread <= 2 ? spread : 2;
    }
    ~TextWeightScope() {
      renderer_.textCoverageCurve_ = previousCurve_;
      renderer_.textSpread_ = previousSpread_;
    }
    TextWeightScope(const TextWeightScope&) = delete;
    TextWeightScope& operator=(const TextWeightScope&) = delete;

   private:
    const GfxRenderer& renderer_;
    const uint8_t* previousCurve_;
    uint8_t previousSpread_;
  };
  const uint8_t* textCoverageCurve() const { return textCoverageCurve_; }
  uint8_t textSpread() const { return textSpread_; }

  class TextInkScope {
   public:
    TextInkScope(const GfxRenderer& renderer, uint8_t level) : renderer_(renderer), previous_(renderer.textInkLevel_) {
      renderer_.textInkLevel_ = std::min<uint8_t>(level, 15);
    }
    ~TextInkScope() { renderer_.textInkLevel_ = previous_; }
    TextInkScope(const TextInkScope&) = delete;
    TextInkScope& operator=(const TextInkScope&) = delete;

   private:
    const GfxRenderer& renderer_;
    uint8_t previous_;
  };
  uint8_t textInkLevel(bool black = true) const { return black ? textInkLevel_ : 15; }

  class SyntheticBoldScope {
   public:
    SyntheticBoldScope(const GfxRenderer& renderer, const uint8_t pixels)
        : renderer_(renderer), previous_(renderer.syntheticBoldPixels) {
      renderer_.syntheticBoldPixels = pixels <= MAX_SYNTHETIC_BOLD_PIXELS ? pixels : MAX_SYNTHETIC_BOLD_PIXELS;
    }
    ~SyntheticBoldScope() { renderer_.syntheticBoldPixels = previous_; }
    SyntheticBoldScope(const SyntheticBoldScope&) = delete;
    SyntheticBoldScope& operator=(const SyntheticBoldScope&) = delete;

   private:
    const GfxRenderer& renderer_;
    uint8_t previous_;
  };

  // Layout may use advance-only SD font tables; rendered measurement includes kerning and ligatures.
  enum class TextMeasureMode { Layout, Rendered };
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns kerning plus optional tracking between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style, int8_t tracking = 0) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode mode = TextMeasureMode::Layout) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  int getLineHeight(int fontId, float compression) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(RenderMode mode);
  RenderMode getRenderMode() const { return renderMode; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH,
                            DisplayRefreshContext context = DisplayRefreshContext::Normal) const;
  bool displayGrayscaleBase(HalDisplay::GrayscaleMode mode,
                            HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
  // Active input encoding, used when drawing monochrome overlays into planes.
  bool grayPlanesAreAbsolute() const { return absoluteGrayPlanes; }

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;
  bool combinesGrayscaleBase() const;
  bool supportsTextOnlyCombinedBase() const;
  bool supportsReaderTransitions() const;
  bool supportsContinuousImageReading() const;
  bool canUseTextTransition() const;
  void cancelGrayscale() const;
  bool storeBwBuffer();  // Returns true if buffer was stored successfully
  // Restore and free the stored buffer. resyncPanelBaseline rewrites the
  // controller's differential baseline to the restored frame — correct after
  // a grayscale render (the glass matches the stored BW plane), WRONG when
  // the glass shows content painted after the store (overlay chrome): the
  // next differential would treat that content as already erased and leave
  // it on the glass. Such callers pass false so the baseline keeps tracking
  // what was last pushed.
  void restoreBwBuffer(bool resyncPanelBaseline = true);
  // Free a stored buffer without restoring it (the page under it changed).
  void discardStoredBwBuffer() { freeBwBufferChunks(); }
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Lend the 48 KB framebuffer's bytes to a memory-hungry phase (chapter
  // builds) WITHOUT freeing the allocation, so it never moves and repeated
  // loans cannot fragment the heap. Between release and restore NOTHING may
  // draw or display — the panel keeps showing its last refreshed image. The
  // lent bytes are published via buildscratch::claim() for consumers like
  // InflateStream. restore returns the buffer white, so the caller must
  // redraw the full screen; it cannot fail (no allocation involved).
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }

  // RAII form of the loan above, for blocking build regions with early-return
  // error paths: restores on scope exit (or explicitly via end()). Display the
  // popup/screen the panel should hold BEFORE constructing one. Constructing
  // while the framebuffer is already lent yields an inert loan (nesting-safe).
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer);
    ~FrameBufferLoan() { end(); }
    void end();
    FrameBufferLoan(const FrameBufferLoan&) = delete;
    FrameBufferLoan& operator=(const FrameBufferLoan&) = delete;

   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint8_t getGrayscaleLevels() const;
  bool beginGrayscale16();
  bool beginRetainedGrayscale16();
  // Logical navigation geometry from the published page layout, rotated once
  // at the renderer/HAL boundary. Empty bar means a subpage without main tabs.
void setNextGray16UiRefresh(int x, int y, int width, int height, uint8_t contentEvery, uint8_t smallEvery,
                              bool adaptive = false) const;
  // Current logical text box; unchanged chrome outside it holds during a turn.
  void setNextGray16ReaderRefresh(int x, int y, int width, int height) const;
  bool commitGrayscale16() const;
  void cancelGrayscale16() const;
  bool isGrayscale16Active() const { return grayscale16Buffer != nullptr; }
  // In portrait a logical row runs down a panel column, so consecutive x pixels are a
  // whole panel row apart in memory (a PSRAM cache miss each). Bulk 16-level writers walk
  // logical columns instead when this is true.
  bool grayscale16PrefersColumns() const { return orientation == Portrait || orientation == PortraitInverted; }
  // The 16-level value (0 = black .. 15 = white) at a logical pixel of the active frame;
  // 15 outside the frame or with none active. For blending over a picture.
  uint8_t grayscale16Level(int x, int y) const;
  void antialiasedQuarter(int radius, int cx, int cy, int xDir, int yDir, int stroke, bool ink) const;
  void setDisplayCapture(bool capture) { displayCapture_ = capture; }
  bool isDisplayCapture() const { return displayCapture_; }
  // Render into a caller-owned 4bpp frame (e.g. a PSRAM page cache) instead of the
  // panel's; nothing is pushed. The B/W proxy still goes to the framebuffer, which
  // the caller saves and restores. End with endGrayscale16Offscreen().
  bool beginGrayscale16Offscreen(uint8_t* frame);
  void endGrayscale16Offscreen() { grayscale16Buffer = nullptr; }
  uint8_t* grayscale16Frame() const { return grayscale16Buffer; }
  size_t grayscale16FrameBytes() const { return static_cast<size_t>(panelWidth / 2) * panelHeight; }
  void drawGrayscale16Pixel(int x, int y, uint8_t gray) const;
  // Fill a logical rectangle with one 16-level tone (0 = black ... 15 = white) and its
  // B/W proxy, row by row in panel memory. Same pixels as drawGrayscale16Pixel() over
  // the rectangle, without a rotation and two read-modify-writes per pixel: on a
  // rotated panel those walk PSRAM column-wise and dominated UI paint time.
  void fillGrayscale16Rect(int x, int y, int width, int height, uint8_t level) const;
  // Byte-exact snapshot of a logical rectangle in both panel buffers (16-level frame and
  // B/W proxy), so unchanged artwork such as a cover is written back row by row instead
  // of being read, decoded and drawn again. The caller owns the storage.
  struct PanelTile {
    int16_t lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;  // logical, half-open, after clipping
    int16_t px0 = 0, py0 = 0, px1 = 0, py1 = 0;  // physical, inclusive
    uint8_t orientation = 0;
    size_t bytes() const;
  };
  // False when no 16-level frame is active or nothing of the rectangle is visible.
  bool panelTileFor(int x, int y, int width, int height, PanelTile* tile) const;
  void capturePanelTile(const PanelTile& tile, uint8_t* out) const;
  // False (nothing written) unless the same pixels are visible now as at capture.
  bool restorePanelTile(const PanelTile& tile, const uint8_t* in) const;
  void blendGrayscale16Pixel(int x, int y, uint8_t inkLevel, uint8_t coverage) const;
  // Copy a rectangle of the B/W proxy into the native 16-gray frame (ink -> black,
  // paper -> white), so text and shapes drawn with the normal B/W API can sit on a
  // 16-gray picture. `cornerRadius` leaves the corners outside a rounded rectangle
  // untouched. No-op unless a 16-gray frame is open.
  void copyBwToGrayscale16(int x, int y, int width, int height, int cornerRadius = 0) const;
  bool drawBitmapGrayscale16(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                             float cropY = 0) const;
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};

static_assert(!GfxRenderer::mapTwoBitPixel(GfxRenderer::GRAYSCALE_MSB, 0).draw);
static_assert(GfxRenderer::mapTwoBitPixel(GfxRenderer::GRAYSCALE_MSB, 1).draw);
static_assert(GfxRenderer::mapTwoBitPixel(GfxRenderer::GRAYSCALE_MSB, 2).draw);
static_assert(!GfxRenderer::mapTwoBitPixel(GfxRenderer::GRAYSCALE_LSB, 0).draw);
static_assert(GfxRenderer::mapTwoBitPixel(GfxRenderer::GRAYSCALE_LSB, 1).draw);
static_assert(!GfxRenderer::mapTwoBitPixel(GfxRenderer::GRAYSCALE_LSB, 2).draw);
