#include "GfxRenderer.h"

#include <BidiUtils.h>
#include <BoardConfig.h>
#include <BuildScratch.h>
#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <MemoryManager.h>
#include <MissingGlyph.h>
#include <SdCardFont.h>
#include <TtfEpdFont.h>
#include <Utf8.h>

#include <algorithm>

// Named fill grays on the 16-level frame; RICKYOS_GRAY_PRESET (RickyDesignTokens.h) deepens them.
#ifndef RICKYOS_GRAY_PRESET
#define RICKYOS_GRAY_PRESET 3
#endif
#if FREEINK_DEVICE_METALIO_PLUS
static constexpr bool kPlusUiTone = true;
#else
static constexpr bool kPlusUiTone = false;
#endif
static constexpr uint8_t kLightGrayLevel =
    RICKYOS_GRAY_PRESET == 3 ? 8
    : RICKYOS_GRAY_PRESET == 2 ? (kPlusUiTone ? 4 : 7)
    : RICKYOS_GRAY_PRESET == 1 ? 9 : 11;
static constexpr uint8_t kDarkGrayLevel =
    RICKYOS_GRAY_PRESET == 3 ? 5
    : RICKYOS_GRAY_PRESET == 2 ? (kPlusUiTone ? 2 : 4)
    : RICKYOS_GRAY_PRESET == 1 ? 6 : 7;
#include <memory>
#include <new>
#include <string_view>

#include "../Memory/Memory.h"
#include "FontCacheManager.h"
#include "GlyphBitmap.h"
#include "Memory.h"

namespace {
constexpr int trackingBetween(const uint32_t leftCp, const uint32_t rightCp, const int8_t tracking) {
  const auto isSpace = [](const uint32_t cp) { return cp == ' ' || cp == 0xA0 || cp == 0x3000; };
  return leftCp == 0 || isSpace(leftCp) || isSpace(rightCp) ? 0 : tracking;
}

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}

template <typename Display>
bool combinesGrayscaleBase(const Display& display) {
  if constexpr (requires { display.combinesGrayscaleBase(); }) return display.combinesGrayscaleBase();
  return false;
}
template <typename Display>
bool supportsTextOnlyCombinedBase(const Display& display) {
  if constexpr (requires { display.supportsTextOnlyCombinedBase(); }) return display.supportsTextOnlyCombinedBase();
  return combinesGrayscaleBase(display);
}
template <typename Display>
uint8_t grayscaleLevels(const Display& display) {
  if constexpr (requires { display.getGrayscaleLevels(); }) return display.getGrayscaleLevels();
  return 4;  // Compatibility with HALs that expose only four-level images.
}
template <typename Display>
uint8_t* beginNativeGray(Display& display) {
  if constexpr (requires { display.beginGrayscale16(); }) return display.beginGrayscale16();
  return nullptr;
}
template <typename Display>
bool commitNativeGray(Display& display) {
  if constexpr (requires { display.commitGrayscale16(); }) return display.commitGrayscale16();
  return false;
}
template <typename Display>
uint8_t* beginRetainedNativeGray(Display& display) {
  if constexpr (requires { display.beginRetainedGrayscale16(); }) return display.beginRetainedGrayscale16();
  return nullptr;
}
template <typename Display>
void cancelNativeGray(Display& display) {
  if constexpr (requires { display.cancelGrayscale16(); }) display.cancelGrayscale16();
}

template <typename Display>
void cancelGrayscale(Display& display) {
  if constexpr (requires { display.cancelGrayscale(); }) display.cancelGrayscale();
}

uint16_t getSdCardSpaceAdvance(SdCardFont& font, const EpdFontFamily::Style style) {
  const uint8_t resolvedStyle = resolveSdCardStyle(font, style);
  const uint16_t advance = font.getAdvance(' ', resolvedStyle);
  if (advance != 0) return advance;

  // Zero means uncached (full table, style not prewarmed, or failed
  // preparation): read the glyph, as the per-codepoint slow path does.
  const EpdFont* epdFont = font.getEpdFont(resolvedStyle);
  const EpdGlyph* glyph = epdFont ? epdFont->getGlyph(' ') : nullptr;
  return glyph ? glyph->advanceX : 0;
}
}  // namespace

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, BidiUtils::BidiBaseDir baseDir);

// Appends the shaped visual form of every RTL token in `text` to `shapedOut`.
// getTextAdvanceX() measures the bidi-reordered, Arabic-shaped codepoint stream,
// so the SD advance table must be warmed with the presentation forms as well as
// the logical codepoints — otherwise every RTL word measurement misses the fast
// path and falls through to onGlyphMiss(), which opens the .cpfont and reads
// glyph metadata + bitmap into the 8-slot overflow ring, once per glyph.
// Tokens without RTL lead bytes (0xD6-0xDB) are skipped with a byte scan, so
// pure-LTR text pays almost nothing.
void appendShapedRtlTokens(const char* text, std::string& shapedOut) {
  const auto isBreak = [](const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  std::string token;
  std::string visual;
  const char* p = text;
  while (*p) {
    while (*p && isBreak(*p)) ++p;
    const char* start = p;
    bool hasRtlBytes = false;
    while (*p && !isBreak(*p)) {
      const auto b = static_cast<unsigned char>(*p);
      hasRtlBytes = hasRtlBytes || (b >= 0xD6 && b <= 0xDB);
      ++p;
    }
    if (!hasRtlBytes) continue;
    token.assign(start, p - start);
    if (BidiUtils::applyBidiVisual(token.c_str(), visual, static_cast<int>(BidiUtils::BidiBaseDir::AUTO))) {
      shapedOut += visual;
    }
  }
}
}  // namespace

#if RICKYOS_PAINT_PROFILE
namespace {
// Glyph fetches are timed on their own: the nesting-aware scopes would fold them into Text.
struct GlyphFetchTimer {
  uint32_t start = micros();
  ~GlyphFetchTimer() {
    auto& profile = GfxRenderer::paintProfile();
    profile.us[static_cast<int>(GfxRenderer::PaintKind::Glyph)] += micros() - start;
    ++profile.calls[static_cast<int>(GfxRenderer::PaintKind::Glyph)];
  }
};
}  // namespace
#endif

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
#if RICKYOS_PAINT_PROFILE
  const GlyphFetchTimer glyphFetchTimer;
#endif
  // Vector (TTF) fonts: the glyph bitmap lives in the font's own cache, keyed by
  // the EpdGlyph the miss handler returned. Checked first so it never reaches the
  // SdCardFont overflow cast below. nullptr = zero-width glyph (e.g. space).
  if (fontData->vectorBitmapHandler != nullptr) {
    return fontData->vectorBitmapHandler(fontData->glyphMissCtx, glyph);
  }
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  // Preload must target the face the text is drawn in, or the glyphs never reach the
  // SD font's resident cache and the text renders blank.
  fontId = resolveFontFamilyId(fontId);
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    std::string shaped;
    appendShapedRtlTokens(utf8Text, shaped);
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
    return;
  }
  // TTF (vector) fonts need nothing here: getGlyph faults glyphs in on demand
  // (glyphMissHandler), and the page's set is batch-warmed by the render scan's
  // prewarmCache(). Pre-faulting a whole chapter's text during layout would just
  // thrash the page-bounded cache, so it is intentionally omitted.
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* const* segments, const size_t* segmentLens,
                                        const size_t segmentCount, const bool includeSpace, const bool includeHyphen,
                                        const uint8_t styleMask) const {
  fontId = resolveFontFamilyId(fontId);
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    std::string shaped;
    for (size_t seg = 0; seg < segmentCount; seg++) {
      const char* p = segments[seg];
      const char* const end = p + segmentLens[seg];
      while (p < end) {
        appendShapedRtlTokens(p, shaped);
        p += strlen(p) + 1;
      }
    }
    int missed = it->second->buildAdvanceTablePacked(segments, segmentLens, segmentCount, includeSpace, includeHyphen,
                                                     styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
    return;
  }
  // TTF (vector) fonts: nothing to do — glyphs fault in on demand at getGlyph
  // and the page is batch-warmed by the render scan (see the other overload).
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);

  // Evictable-cache sinks for the SDK memory manager: layout's OOM choke
  // points (WordStore/TextBlock) call ensureFree() to flush these and retry
  // before failing a section build. Both caches rebuild transparently from SD
  // or flash on the next glyph access. The SD-font persistent advance table is
  // deliberately NOT a sink: evicting it mid-paragraph would make later
  // measurements in the same layout pass return 0-width advances.
  auto& memoryManager = freeink::MemoryManager::instance();
  memoryManager.registerSink({"gfx.renderGlyphCache", 50, [this](size_t) -> size_t {
                                if (!fontCacheManager_ || fontCacheManager_->isScanning()) return 0;
                                const size_t before = freeink::MemoryManager::instance().freeBytes();
                                fontCacheManager_->clearCache();
                                const size_t after = freeink::MemoryManager::instance().freeBytes();
                                return after > before ? after - before : 0;
                              }});
  memoryManager.registerSink({"gfx.sdFontMini", 60, [this](size_t) -> size_t {
                                const size_t before = freeink::MemoryManager::instance().freeBytes();
                                for (auto& entry : sdCardFonts_) {
                                  if (entry.second) entry.second->clearCache();
                                }
                                const size_t after = freeink::MemoryManager::instance().freeBytes();
                                return after > before ? after - before : 0;
                              }});
#if CROSSPOINT_VECTOR_FONTS
  // TTF glyph arenas are safe to shed mid-layout, unlike the SD-font advance
  // table: metrics re-fault through FreeType with identical values, so layout
  // cannot silently corrupt — the cost is re-rasterizing on the next draw.
  memoryManager.registerSink({"gfx.ttfGlyphArenas", 70, [this](size_t) -> size_t {
                                const size_t before = freeink::MemoryManager::instance().freeBytes();
                                for (auto& entry : ttfFonts_) {
                                  if (entry.second) entry.second->releaseResidentCaches();
                                }
                                const size_t after = freeink::MemoryManager::instance().freeBytes();
                                return after > before ? after - before : 0;
                              }});
#endif
}

void GfxRenderer::releaseFrameBufferForBuild() {
  // Lend the framebuffer's bytes IN PLACE: the allocation is never freed, so
  // it cannot move and repeated loans cannot fragment the heap (the previous
  // free+realloc model measurably decayed the max contiguous block over a
  // session). The bytes are deposited in the build-scratch registry so
  // memory-hungry build phases (e.g. InflateStream's tinfl state + window)
  // can claim them instead of allocating.
  uint32_t size = 0;
  uint8_t* scratch = display.lendFrameBufferStorage(&size);
  frameBuffer = nullptr;
  if (scratch) {
    buildscratch::lend(scratch, size);
  }
}

bool GfxRenderer::restoreFrameBufferAfterBuild() {
  buildscratch::reclaim();
  display.returnFrameBufferStorage();  // cannot fail: the allocation was never freed
  frameBuffer = display.getFrameBuffer();
  return frameBuffer != nullptr;
}

GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer) : renderer_(renderer) {
  // Nesting guard: if the framebuffer is already lent out (an outer loan),
  // stay inert so this end() cannot return storage the outer loan still owns.
  if (!renderer_.hasFrameBuffer()) return;
  renderer_.releaseFrameBufferForBuild();
  active_ = true;
}

void GfxRenderer::FrameBufferLoan::end() {
  if (!active_) return;
  active_ = false;
  if (!renderer_.restoreFrameBufferAfterBuild()) {
    // Only reachable if the framebuffer never existed, which begin() already
    // asserts against; kept as a backstop since running blind helps nobody.
    LOG_ERR("GFX", "Framebuffer restore failed - restarting");
    ESP.restart();
  }
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

int GfxRenderer::resolveFontFamilyId(const int fontId) const {
  const auto it = preferredFontMap_.find(fontId);
  const int candidate = (it != preferredFontMap_.end()) ? it->second : fontId;
  if (fontMap.find(candidate) != fontMap.end()) return candidate;
  return fontId;
}

int GfxRenderer::resolveTextFontId(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  // A target may rebind a UI id to the SD family its UI is drawn in (setPreferredFont).
  // Resolving it here, instead of swapping fontMap entries, keeps measuring and drawing
  // on one face -- both call this function.
  const int effectiveFontId = resolveFontFamilyId(fontId);

  // Fallbacks stay keyed by the requested id; for a rebound id the entry names the
  // built-in family, which is exactly what the SD face should fall back to.
  const auto fbIt = fallbackFontMap_.find(fontId);
  if (fbIt == fallbackFontMap_.end() || text == nullptr || *text == '\0') {
    return effectiveFontId;
  }
  const auto fontIt = fontMap.find(effectiveFontId);
  if (fontIt == fontMap.end()) return effectiveFontId;
  const EpdFontFamily& primary = fontIt->second;
  // UI subsets can cover only part of a filename. Choose one face for the
  // complete run, following the existing CJK subset -> common CJK -> RTL chain.
  // Eight IDs bound the registered UI/SD chain; duplicate IDs also break cycles.
  std::array<int, 8> candidates{fontId};
  size_t count = 1;
  int bestFontId = effectiveFontId;
  size_t bestMissing = 0;
  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
    if (!primary.hasCodepoint(cp, style)) ++bestMissing;
  }
  if (bestMissing == 0) return effectiveFontId;
  for (size_t i = 0; i < count; ++i) {
    const auto chain = fallbackFontMap_.find(candidates[i]);
    if (chain != fallbackFontMap_.end()) {
      for (const int id : chain->second) {
        if (id != 0 && count < candidates.size() &&
            std::find(candidates.begin(), candidates.begin() + count, id) == candidates.begin() + count) {
          candidates[count++] = id;
        }
      }
    }
    if (i == 0 && effectiveFontId == fontId) continue;
    const auto candidate = fontMap.find(candidates[i]);
    if (candidate == fontMap.end()) continue;
    size_t missing = 0;
    cursor = text;
    while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
      if (!candidate->second.hasCodepoint(cp, style)) ++missing;
    }
    if (missing < bestMissing) {
      bestMissing = missing;
      bestFontId = candidates[i];
      if (missing == 0) return bestFontId;
    }
  }
  return bestFontId;
}

void GfxRenderer::prewarmFallbackText(const int fontId, const TextGetter getter, const void* ctx,
                                      const uint32_t textCount, const EpdFontFamily::Style style) const {
  if (getter == nullptr || textCount == 0) return;

  auto sdIt = sdCardFonts_.end();
  for (uint32_t i = 0; i < textCount && sdIt == sdCardFonts_.end(); i++) {
    const char* text = getter(ctx, i);
    if (text == nullptr || *text == '\0') continue;
    const int fallbackFontId = resolveTextFontId(fontId, text, style);
    if (fallbackFontId != fontId) sdIt = sdCardFonts_.find(fallbackFontId);
  }
  if (sdIt == sdCardFonts_.end()) return;

  struct BatchContext {
    TextGetter getter;
    const void* context;
    uint32_t count;
  } batch{getter, ctx, textCount};
  const auto withEllipsis = [](const void* context, const uint32_t index) -> const char* {
    const auto* value = static_cast<const BatchContext*>(context);
    return index < value->count ? value->getter(value->context, index) : "\xe2\x80\xa6";
  };
  const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
  sdIt->second->prewarm(withEllipsis, &batch, textCount + 1, styleMask, false, false);
}

void GfxRenderer::prewarmFallbackText(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return;
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  if (resolvedFontId != fontId) ensureSdGlyphsResident(resolvedFontId, text, style, false);
}

void GfxRenderer::ensureSdGlyphsResident(int fontId, const char* text, const EpdFontFamily::Style style,
                                         const bool metadataOnly) const {
  // Residency must be tracked against the face the text is drawn in.
  fontId = resolveFontFamilyId(fontId);
  const auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt == sdCardFonts_.end()) return;
  const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
  sdIt->second->prewarm(text, styleMask, metadataOnly, false);
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
#if RICKYOS_PAINT_PROFILE
#define RICKY_PAINT_SCOPE(kind) const GfxRenderer::PaintScope paintScope_(GfxRenderer::PaintKind::kind)
namespace {
uint8_t g_paintDepth = 0;
}  // namespace
GfxRenderer::PaintProfile& GfxRenderer::paintProfile() {
  static PaintProfile profile;
  return profile;
}
GfxRenderer::PaintScope::PaintScope(const PaintKind kind) : kind_(kind), outer_(g_paintDepth++ == 0) {
  if (outer_) start_ = micros();
}
GfxRenderer::PaintScope::~PaintScope() {
  --g_paintDepth;
  if (!outer_) return;
  auto& profile = paintProfile();
  profile.us[static_cast<int>(kind_)] += micros() - start_;
  ++profile.calls[static_cast<int>(kind_)];
}
#else
#define RICKY_PAINT_SCOPE(kind) ((void)0)
#endif

static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

// Output of screenRectToAlignedMemRect: a rectangle in panel-memory
// coordinates whose x and width are guaranteed to be multiples of 8 (the
// SDK's EInkDisplay::displayWindow alignment requirement). `valid == false`
// means the input was empty or fully outside the panel.
struct AlignedMemRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool valid = false;
};

// Translate a screen-coordinate rectangle (the coordinate system used by
// fillRect / drawText / the rest of the renderer's public API) into a
// panel-memory rectangle suitable for direct framebuffer indexing. Rotates
// the rectangle's two opposite corners with rotateCoordinates(), takes the
// bounding box (which naturally swaps width/height in Portrait /
// PortraitInverted), then snaps the x extent outward to multiples of 8 and
// clamps to panel bounds. Precondition: panel dims are multiples of 8 (true
// for the 800x480 panel), so clamping cannot re-break alignment.
static AlignedMemRect screenRectToAlignedMemRect(GfxRenderer::Orientation orientation, int sx, int sy, int sw, int sh,
                                                 uint16_t panelWidth, uint16_t panelHeight) {
  AlignedMemRect out;
  if (sw <= 0 || sh <= 0) return out;

  int x0, y0, x1, y1;
  rotateCoordinates(orientation, sx, sy, &x0, &y0, panelWidth, panelHeight);
  rotateCoordinates(orientation, sx + sw - 1, sy + sh - 1, &x1, &y1, panelWidth, panelHeight);

  const int memXLo = std::min(x0, x1);
  const int memYLo = std::min(y0, y1);
  const int memXHi = std::max(x0, x1) + 1;  // exclusive upper bound
  const int memYHi = std::max(y0, y1) + 1;

  // Snap x outward to multiples of 8.
  int alignedXLo = memXLo & ~0x7;        // round down
  int alignedXHi = (memXHi + 7) & ~0x7;  // round up

  if (alignedXLo < 0) alignedXLo = 0;
  if (alignedXHi > panelWidth) alignedXHi = panelWidth;
  int clampedYLo = memYLo;
  int clampedYHi = memYHi;
  if (clampedYLo < 0) clampedYLo = 0;
  if (clampedYHi > panelHeight) clampedYHi = panelHeight;

  if (alignedXHi <= alignedXLo || clampedYHi <= clampedYLo) return out;

  out.x = static_cast<uint16_t>(alignedXLo);
  out.y = static_cast<uint16_t>(clampedYLo);
  out.w = static_cast<uint16_t>(alignedXHi - alignedXLo);
  out.h = static_cast<uint16_t>(clampedYHi - clampedYLo);
  out.valid = true;
  return out;
}

enum class TextRotation { None, Rotated90CW };

static void drawGlyphPixel(const GfxRenderer& renderer, const int x, const int y, const bool pixelState,
                           const uint8_t syntheticBoldPixels) {
  renderer.drawPixel(x, y, pixelState);
  if (syntheticBoldPixels == 0) return;
  renderer.drawPixel(x + 1, y, pixelState);
  if (syntheticBoldPixels >= 2) renderer.drawPixel(x + 2, y, pixelState);
  if (syntheticBoldPixels >= 3) renderer.drawPixel(x + 3, y, pixelState);
}

constexpr uint8_t dilate2BitCoverage(const uint8_t current, const uint8_t previous1, const uint8_t previous2,
                                     const uint8_t previous3, const uint8_t pixels) {
  uint8_t darkest = current;
  if (pixels >= 1 && previous1 > darkest) darkest = previous1;
  if (pixels >= 2 && previous2 > darkest) darkest = previous2;
  if (pixels >= 3 && previous3 > darkest) darkest = previous3;
  return darkest;
}

static_assert(dilate2BitCoverage(0, 1, 0, 0, 1) == 1);  // Light extends one pixel.
static_assert(dilate2BitCoverage(3, 1, 0, 0, 1) == 3);  // A gray neighbor cannot lighten black.
static_assert(dilate2BitCoverage(0, 0, 3, 0, 2) == 3);  // Standard extends two pixels.
static_assert(dilate2BitCoverage(0, 0, 0, 3, 3) == 3);  // Heavy extends three pixels.
static_assert(dilate2BitCoverage(0, 3, 3, 3, 0) == 0);  // Off preserves the original coverage.

static void draw2BitGlyphPixel(const GfxRenderer& renderer, const GfxRenderer::RenderMode renderMode, const int x,
                               const int y, const bool pixelState, const uint8_t coverage) {
  const auto pixel = GfxRenderer::mapTwoBitGlyphCoverage(renderMode, coverage);
  if (!pixel.draw) return;
  renderer.drawPixel(x, y, renderMode == GfxRenderer::BW ? pixelState : pixel.state);
}

// 4 位覆盖度：两个像素一字节，高半字节在前（与 .cpfont converter 的 4 位布局一致）。
static uint8_t get4BitCoverage(const uint8_t* bitmap, const int pixelPosition) {
  const uint8_t byte = bitmap[pixelPosition >> 1];
  return (byte >> ((1 - (pixelPosition & 1)) * 4)) & 0xF;
}

static uint8_t get2BitCoverage(const uint8_t* bitmap, const int pixelPosition, const bool fourBit = false) {
  if (fourBit) return get4BitCoverage(bitmap, pixelPosition) >> 2;
  const uint8_t byte = bitmap[pixelPosition >> 2];
  return (byte >> ((3 - (pixelPosition & 3)) * 2)) & 0x3;
}

// Coverage after the text-weight curve (TextWeightScope), on the 0..15 scale and on the
// 0..3 scale the 2-bit paths use. A 2-bit sample is spread to 0/5/10/15 first.
static uint8_t weigh4BitCoverage(const uint8_t* curve, const uint8_t coverage) {
  return curve ? curve[coverage & 0xF] : coverage;
}

static uint8_t weighted2BitCoverage(const uint8_t* curve, const uint8_t* bitmap, const int pixelPosition,
                                    const bool fourBit) {
  if (!curve) return get2BitCoverage(bitmap, pixelPosition, fourBit);
  const uint8_t coverage = fourBit ? get4BitCoverage(bitmap, pixelPosition)
                                   : static_cast<uint8_t>(get2BitCoverage(bitmap, pixelPosition) * 5);
  return static_cast<uint8_t>((curve[coverage] + 2) / 5);
}

// 把 4 位覆盖度写进 16 级灰度缓冲。drawGrayscale16Pixel() 的 gray 是 0 = 黑、255 = 白，
// 而覆盖度是 0 = 全透明、15 = 满墨，所以要取反。
//
// 覆盖度 0 必须**跳过**而不是画成白：那些像素不属于字形，画上去会把底图涂掉 —— 16 级
// 缓冲是整帧一次推出去的，不像 LSB/MSB 平面那样只表达"这个平面要不要点亮"。
// / Write 4-bit coverage into the 16-level buffer. drawGrayscale16Pixel() takes 0 = black
// and 255 = white while coverage runs 0 = transparent to 15 = full ink, hence the
// inversion. Coverage 0 must be SKIPPED rather than painted white: those pixels are not
// part of the glyph, and the 16-level buffer is pushed as a whole frame, unlike the
// LSB/MSB planes, where a zero bit only means "not this plane".
static void draw4BitGlyphPixel(const GfxRenderer& renderer, const int x, const int y, const uint8_t coverage) {
  if (coverage == 0) return;
  renderer.blendGrayscale16Pixel(x, y, renderer.textInkLevel(), coverage);
}

// Outline fallback has no font bitmap and uses the same metrics as layout.
template <TextRotation rotation = TextRotation::None>
static void renderMissingGlyph(const GfxRenderer& renderer, const EpdFontData& fontData, const uint32_t cp,
                               const int cursorX, const int cursorY, const bool pixelState, const bool scaled = false) {
  const EpdGlyph glyph = missingGlyph::metrics(fontData.ascender, cp);
  if (glyph.width == 0) return;
  const int width = scaled ? (glyph.width + 1) / 2 : glyph.width;
  const int height = scaled ? (glyph.height + 1) / 2 : glyph.height;
  const int left = scaled ? glyph.left / 2 : glyph.left;
  const int top = scaled ? glyph.top / 2 : glyph.top;
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int x = cursorX + fontData.ascender - top;
    const int y = cursorY - left - width + 1;
    if (renderer.glyphIntersectsStrip(x, y, x + height - 1, y + width - 1)) {
      renderer.drawRect(x, y, height, width, pixelState);
    }
  } else {
    const int x = cursorX + left;
    const int y = cursorY - top;
    if (renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
      renderer.drawRect(x, y, width, height, pixelState);
    }
  }
}

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
// Render a glyph at 50% scale. Used for SUP/SUB style bits.
//
// Each destination pixel represents a 2x2 source block. Drawing when that block
// contains ink preserves thin strokes that nearest-neighbor sampling can skip.
//
// The advance width is also halved in drawText() so layout reserves exactly the right
// horizontal space for the scaled glyph.
static void renderCharScaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                             const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                             const bool pixelState, const EpdFontFamily::Style style,
                             const uint8_t syntheticBoldPixels) {
  if (renderer.grayPlanesAreAbsolute()) renderMode = GfxRenderer::BW;
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  const EpdFontData* fontData = fontFamily.getData(style);
  if (!glyph) {
    renderMissingGlyph(renderer, *fontData, cp, cursorX, cursorY, pixelState, true);
    return;
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;  // read after the fetch: a vector glyph's box arrives with it
  const int srcH = glyph->height;
  const int dstW = (srcW + 1) / 2;  // ceil so odd-width glyphs aren't clipped
  const int dstH = (srcH + 1) / 2;
  // Scale the glyph bearing by the same factor so the scaled glyph sits at the correct
  // pixel offset from the (already-shifted) cursor position.
  const int baseX = cursorX + glyph->left / 2;
  const int baseY = cursorY - glyph->top / 2;

  if (fontData->is2Bit || fontData->is4Bit) {
    // 2-bit packed format: 4 pixels per byte, MSB first, 2 bits per pixel.
    // raw value: 0=white, 1=light-gray, 2=dark-gray, 3=black.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        uint8_t coverage = 0;
        uint8_t maxRaw = 0;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t raw = get2BitCoverage(bitmap, pos, fontData->is4Bit);
            coverage += raw;
            if (raw > maxRaw) maxRaw = raw;
          }
        }
        if (maxRaw >= 2 || coverage >= 2) {
          drawGlyphPixel(renderer, baseX + dstX, baseY + dstY, pixelState, syntheticBoldPixels);
        }
      }
    }
  } else {
    // 1-bit packed format: 8 pixels per byte, MSB first.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 3];
            const uint8_t bit = 7 - (pos & 7);
            if ((byte >> bit) & 1) {
              hasInk = true;
            }
          }
        }
        if (hasInk) {
          drawGlyphPixel(renderer, baseX + dstX, baseY + dstY, pixelState, syntheticBoldPixels);
        }
      }
    }
  }
}

template <TextRotation rotation = TextRotation::None>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style,
                           const uint8_t syntheticBoldPixels = 0) {
  if (renderer.grayPlanesAreAbsolute()) renderMode = GfxRenderer::BW;
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  const EpdFontData* fontData = fontFamily.getData(style);
  if (!glyph) {
    renderMissingGlyph<rotation>(renderer, *fontData, cp, cursorX, cursorY, pixelState);
    return;
  }

  // A vector glyph may not have its box yet (TTF layout takes advances only); fetching
  // its bitmap first fills the box in. No-op for every other font.
  if (glyph->dataLength == 0 && fontData->vectorBitmapHandler) (void)renderer.getGlyphBitmap(fontData, glyph);
  const bool is2Bit = fontData->is2Bit;
  const bool is4Bit = fontData->is4Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    const int grow = renderer.textSpread() != 0 ? 1 : 0;  // a spread stroke reaches one pixel further
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1 + syntheticBoldPixels + grow,
                                       gy0 + height - 1 + grow)) {
      return;
    }
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (bitmap == nullptr) return;
  const uint8_t* const curve = renderer.textCoverageCurve();

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    const uint8_t spread = renderer.textSpread();
    if (spread != 0 && (is2Bit || is4Bit) && syntheticBoldPixels == 0) {
      // Text weight past the font's own (TextWeightScope): each pixel also takes its left,
      // upper and upper-left neighbours' coverage (halved for a half-pixel spread), so every
      // stroke grows right and down while the 16 gray levels of its edges survive.
      const auto at = [&](const int x, const int y) -> uint8_t {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        const int position = y * width + x;
        return is4Bit ? get4BitCoverage(bitmap, position) : static_cast<uint8_t>(get2BitCoverage(bitmap, position) * 5);
      };
      const bool native16 = renderer.isGrayscale16Active();
      for (int glyphY = 0; glyphY <= height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX <= width; glyphX++) {
          uint8_t neighbour = std::max({at(glyphX - 1, glyphY), at(glyphX, glyphY - 1), at(glyphX - 1, glyphY - 1)});
          if (spread == 1) neighbour = static_cast<uint8_t>(neighbour / 2);
          const uint8_t coverage = weigh4BitCoverage(curve, std::max(at(glyphX, glyphY), neighbour));
          if (coverage == 0) continue;
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }
          if (!native16) {
            draw2BitGlyphPixel(renderer, renderMode, screenX, screenY, pixelState, static_cast<uint8_t>(coverage >> 2));
          } else if (pixelState) {
            draw4BitGlyphPixel(renderer, screenX, screenY, coverage);
          } else {
            renderer.blendGrayscale16Pixel(screenX, screenY, 15, coverage);
          }
        }
      }
    } else if (rotation == TextRotation::None && (is2Bit || is4Bit) && renderer.isGrayscale16Active() &&
               renderer.grayscale16PrefersColumns()) {
      // On a portrait panel a logical row is a panel column: walking the glyph row by row
      // strode a whole panel row (a PSRAM cache miss in both buffers) per pixel, and glyphs
      // were most of a page's paint time. Walk it column by column instead, so consecutive
      // pixels are adjacent in panel memory. Every pixel is blended once and independently,
      // so the frame is the same byte for byte as the row walks below.
      const uint8_t ink = renderer.textInkLevel(pixelState);
      const int outputWidth = width + syntheticBoldPixels;
      const auto coverage2 = [&](const int glyphX, const int glyphY) -> uint8_t {
        return glyphX >= 0 && glyphX < width ? weighted2BitCoverage(curve, bitmap, glyphY * width + glyphX, is4Bit)
                                             : 0;
      };
      for (int glyphX = 0; glyphX < outputWidth; glyphX++) {
        const int screenX = innerBase + glyphX;
        for (int glyphY = 0; glyphY < height; glyphY++) {
          const int screenY = outerBase + glyphY;
          if (is4Bit && syntheticBoldPixels == 0) {
            // Preserve all coverage steps for both ink and selected paper glyphs.
            renderer.blendGrayscale16Pixel(screenX, screenY, ink,
                                           weigh4BitCoverage(curve, get4BitCoverage(bitmap, glyphY * width + glyphX)));
            continue;
          }
          const uint8_t current = coverage2(glyphX, glyphY);
          const uint8_t coverage =
              syntheticBoldPixels == 0
                  ? current
                  : dilate2BitCoverage(current, coverage2(glyphX - 1, glyphY), coverage2(glyphX - 2, glyphY),
                                       coverage2(glyphX - 3, glyphY), syntheticBoldPixels);
          if (pixelState) {
            draw4BitGlyphPixel(renderer, screenX, screenY, static_cast<uint8_t>(coverage * 5));
          } else if (coverage != 0) {
            renderer.blendGrayscale16Pixel(screenX, screenY, 15, static_cast<uint8_t>(coverage * 5));
          }
        }
      }
    } else if (is4Bit && renderer.isGrayscale16Active() && syntheticBoldPixels == 0) {
      // Preserve all coverage steps for both ink and selected paper glyphs.
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }
          renderer.blendGrayscale16Pixel(screenX, screenY, renderer.textInkLevel(pixelState),
                                         weigh4BitCoverage(curve, get4BitCoverage(bitmap, glyphY * width + glyphX)));
        }
      }
    } else if ((is2Bit || is4Bit) && renderer.isGrayscale16Active()) {
      // 2-bit faces (downloaded .cpfont) and synthetic bold go straight into the 16-level
      // frame as well, so a page is rasterised once instead of once per B/W, LSB and MSB
      // plane. Coverage 0..3 spreads over the 16 levels (x5); 0 stays transparent.
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        uint8_t previous1 = 0, previous2 = 0, previous3 = 0;
        const int outputWidth = width + syntheticBoldPixels;
        for (int glyphX = 0; glyphX < outputWidth; glyphX++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }
          const uint8_t current =
              glyphX < width ? weighted2BitCoverage(curve, bitmap, glyphY * width + glyphX, is4Bit) : 0;
          const uint8_t coverage = syntheticBoldPixels == 0 ? current
                                                            : dilate2BitCoverage(current, previous1, previous2,
                                                                                 previous3, syntheticBoldPixels);
          if (pixelState) {
            draw4BitGlyphPixel(renderer, screenX, screenY, static_cast<uint8_t>(coverage * 5));
          } else if (coverage != 0) {
            // White text on ink (a selected pill or tile): coverage lifts toward paper.
            renderer.blendGrayscale16Pixel(screenX, screenY, 15, static_cast<uint8_t>(coverage * 5));
          }
          previous3 = previous2;
          previous2 = previous1;
          previous1 = current;
        }
      }
    } else if (is2Bit || is4Bit) {
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        if (syntheticBoldPixels == 0) {
          for (int glyphX = 0; glyphX < width; glyphX++) {
            int screenX, screenY;
            if constexpr (rotation == TextRotation::Rotated90CW) {
              screenX = outerCoord;
              screenY = innerBase - glyphX;
            } else {
              screenX = innerBase + glyphX;
              screenY = outerCoord;
            }
            draw2BitGlyphPixel(renderer, renderMode, screenX, screenY, pixelState,
                               weighted2BitCoverage(curve, bitmap, glyphY * width + glyphX, is4Bit));
          }
          continue;
        }

        uint8_t previous1 = 0;
        uint8_t previous2 = 0;
        uint8_t previous3 = 0;
        const int outputWidth = width + syntheticBoldPixels;
        for (int glyphX = 0; glyphX < outputWidth; glyphX++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t current = glyphX < width ? weighted2BitCoverage(curve, bitmap, glyphY * width + glyphX, is4Bit)
                                                 : 0;  // White tail extends the edge.
          const uint8_t coverage = dilate2BitCoverage(current, previous1, previous2, previous3, syntheticBoldPixels);
          draw2BitGlyphPixel(renderer, renderMode, screenX, screenY, pixelState, coverage);
          previous3 = previous2;
          previous2 = previous1;
          previous1 = current;
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            if (renderer.isGrayscale16Active()) {
              for (int offset = 0; offset <= syntheticBoldPixels; ++offset)
                renderer.blendGrayscale16Pixel(screenX + offset, screenY, renderer.textInkLevel(pixelState), 15);
            } else {
              drawGlyphPixel(renderer, screenX, screenY, pixelState, syntheticBoldPixels);
            }
          }
        }
      }
    }
  }
}

// Draw an unscaled glyph placed by a logical frame. Equivalent to calling
// drawPixel() for each ink pixel, but the clip test, orientation rotation,
// strip-band check and address math are resolved once per glyph rather than
// once per pixel.
void GfxRenderer::drawGlyphBitmap(const uint8_t* bitmap, const int width, const int height,
                                  const glyphBitmap::Frame& frame, const bool twoBit, const RenderMode mode,
                                  const bool state) const {
  // Apply the logical clip rectangle before rotating, in glyph-local pixels.
  glyphBitmap::Clip clip{0, 0, width, height};
  glyphBitmap::clipToRect(frame, clipLeft_, clipTop_, clipRight_, clipBottom_, clip);

  // Writes go to the framebuffer, or to the strip scratch in tiled grayscale
  // mode; getWriteOriginY()/getWriteRows() bound the rows that exist there.
  glyphBitmap::Target target{getWriteTarget(), panelWidth, panelWidthBytes, getWriteOriginY(), getWriteRows(), {}};
  // Rotate the glyph origin once, then derive the two physical axes by
  // rotating its neighbours along each logical axis. Together these encode
  // the text rotation and the panel orientation as one orthogonal transform.
  glyphBitmap::Frame& physical = target.frame;
  rotateCoordinates(orientation, frame.x, frame.y, &physical.x, &physical.y, panelWidth, panelHeight);
  int nextX, nextY;
  rotateCoordinates(orientation, frame.x + frame.dxX, frame.y + frame.dxY, &nextX, &nextY, panelWidth, panelHeight);
  physical.dxX = nextX - physical.x;
  physical.dxY = nextY - physical.y;
  rotateCoordinates(orientation, frame.x + frame.dyX, frame.y + frame.dyY, &nextX, &nextY, panelWidth, panelHeight);
  physical.dyX = nextX - physical.x;
  physical.dyY = nextY - physical.y;

  const glyphBitmap::Plane plane = mode == BW              ? glyphBitmap::Plane::BW
                                   : mode == GRAYSCALE_MSB ? glyphBitmap::Plane::GrayMSB
                                                           : glyphBitmap::Plane::GrayLSB;
  glyphBitmap::draw(bitmap, width, height, twoBit, plane, state, target, clip);
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  if (x < clipLeft_ || y < clipTop_ || x >= clipRight_ || y >= clipBottom_) return;
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Tiled grayscale: redirect writes to the strip scratch and clip to the
  // current band. Single predictable branch on the hot per-pixel path.
  uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) {
      return;  // pixel outside the band currently being rendered
    }
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  const bool eff = framebufferState(renderMode, state);
  if (eff) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }

  // 16 级通路下，把每一个 1 位写入也镜像进 4bpp 缓冲。
  //
  // 为什么必须：那一趟推到面板的是 4bpp 缓冲，只有字形通过 drawGrayscale16Pixel() 主动
  // 写进去过。状态栏、图标、参考线这些走普通 1 位绘制的东西就只会落在帧缓冲里 —— 推出的
  // 画面里它们直接消失（实测：状态栏整条不见了）。
  //
  // 灰度优先：drawGrayscale16Pixel() 先调这里（镜像成 0/15），随后再把精确灰度写回去，
  // 所以字形的中间调不会被这一步抹平成纯黑或纯白。
  //
  // 代价只有一次分支判断，且只在 16 级缓冲活跃时成立 —— 其它所有渲染路径不受影响。
  // / Mirror every 1-bit write into the 4bpp buffer while the 16-level path is active.
  //
  // Required because that pass pushes the 4bpp buffer, and only glyphs write into it by
  // themselves (via drawGrayscale16Pixel). The status bar, icons and guide lines draw with
  // plain 1-bit primitives and would otherwise exist only in the framebuffer -- they vanish
  // from the pushed frame, which is exactly how the status bar went missing on device.
  //
  // Greyscale wins: drawGrayscale16Pixel() calls this first (mirroring 0/15) and writes the
  // precise level afterwards, so glyph mid-tones are not flattened to black or white.
  //
  // One extra branch on the hot path, taken only when that buffer is active.
  if (grayscale16Buffer != nullptr) {
    const size_t grayIndex = static_cast<size_t>(phyY) * (panelWidth / 2) + (phyX / 2);
    const unsigned grayShift = (phyX & 1) * 4;
    const uint8_t level = eff ? 0u : 15u;
    grayscale16Buffer[grayIndex] = (grayscale16Buffer[grayIndex] & ~(0x0Fu << grayShift)) | (level << grayShift);
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') {
    return 0;
  }

  // Measure with the same font drawText would render with (see resolveTextFontId)
  // so wrapping, truncation and centering of CJK strings stay consistent.

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);
  const int resolvedFontId = resolveTextFontId(fontId, renderedText, style);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  if (resolvedFontId != fontId) ensureSdGlyphsResident(resolvedFontId, renderedText, style, true);

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(renderedText, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style, baseDir)) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir,
                           const int8_t tracking) const {
  RICKY_PAINT_SCOPE(Text);
  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const uint8_t activeSyntheticBoldPixels =
      syntheticBoldPixels != 0 && (style & EpdFontFamily::BOLD) != 0 ? syntheticBoldPixels : 0;
  const auto renderStyle =
      activeSyntheticBoldPixels != 0
          ? static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(style) & ~static_cast<uint8_t>(EpdFontFamily::BOLD))
          : style;

  // Route CJK-bearing strings to the fallback font when the requested font
  // lacks the glyphs (e.g. Chinese book titles drawn with a Latin UI font).

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);
  const int resolvedFontId = resolveTextFontId(fontId, renderedText, renderStyle);

  int yPos = y + getFontAscenderSize(resolvedFontId);
  if (resolvedFontId != fontId) yPos += (getLineHeight(fontId) - getLineHeight(resolvedFontId)) / 2;
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(renderedText, resolvedFontId, renderStyle);
    return;
  }

  if (resolvedFontId != fontId) ensureSdGlyphsResident(resolvedFontId, renderedText, renderStyle, false);

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }
  const auto& font = fontIt->second;

  const char* textCursor = renderedText;
  uint32_t cp;
  uint32_t prevCp = 0;
  bool prevMissing = false;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, renderStyle);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                       combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, renderStyle,
                                         activeSyntheticBoldPixels);
      continue;
    }

    const uint32_t sourceCp = cp;
    cp = font.applyLigatures(cp, textCursor, renderStyle);

    bool usedReplacement = false;
    const EpdGlyph* glyph = font.getGlyph(cp, renderStyle, &usedReplacement);
    if (usedReplacement && fontCacheManager_) {
      fontCacheManager_->reportMissingChineseCodepoint(resolvedFontId, sourceCp);
    }

    const bool missing = glyph == nullptr;
    if (prevCp != 0) {
      const auto kernFP = missing || prevMissing ? 0 : font.getKerning(prevCp, cp, renderStyle);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP) + trackingBetween(prevCp, cp, tracking);
    }
    const EpdGlyph placeholder = missing ? missingGlyph::metrics(font.getData(renderStyle)->ascender, cp) : EpdGlyph{};
    if (missing) glyph = &placeholder;

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point

    const bool isSupSub = (renderStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    if (isSupSub) {
      // Halve the advance so the cursor advances by the same amount the scaled glyph
      // actually occupies, keeping spacing correct without needing a separate smaller font.
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }

    if (isSupSub) {
      // yPos already carries the vertical offset applied by TextBlock::render().
      renderCharScaled(*this, renderMode, font, cp, lastBaseX, yPos, black, renderStyle, activeSyntheticBoldPixels);
    } else {
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, renderStyle,
                                         activeSyntheticBoldPixels);
    }
    prevCp = cp;
    prevMissing = missing;
  }
}

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, const BidiUtils::BidiBaseDir baseDir) {
  if (!text || *text == '\0') return text;

  if (baseDir != BidiUtils::BidiBaseDir::RTL) {
    // Byte-level scan: skip BiDi when no RTL script lead bytes are present.
    // Hebrew UTF-8 lead bytes: 0xD6-0xD7; Arabic/Syriac: 0xD8-0xDB.
    // This covers all RTL content without false negatives and avoids triggering
    // the full UAX#9 algorithm for Latin-extended, em-dashes, accented text, etc.
    bool hasRtlBytes = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
      if (*q >= 0xD6 && *q <= 0xDB) {
        hasRtlBytes = true;
        break;
      }
    }
    if (!hasRtlBytes) return text;
  }

  if (BidiUtils::applyBidiVisual(text, visualBuffer, static_cast<int>(baseDir)) && !visualBuffer.empty()) {
    return visualBuffer.c_str();
  }
  return text;
}
}  // namespace

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth && width - 2 * i > 0 && height - 2 * i > 0; i++) {
    drawRect(x + i, y + i, width - 2 * i, height - 2 * i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  if (grayscale16Buffer && maxRadius > 0) {
    antialiasedQuarter(maxRadius, cx, cy, xDir, yDir, std::max(1, std::min(lineWidth, maxRadius)),
                       framebufferState(renderMode, state));
    return;
  }
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    fillRect(left, py, width, 1, state);
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  RICKY_PAINT_SCOPE(Shape);
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  const bool eff = framebufferState(renderMode, state);
  if (eff) {
    fillRectImpl<Color::Black>(x, y, width, height);
  } else {
    fillRectImpl<Color::White>(x, y, width, height);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  switch (color) {
    case Color::Clear:
      break;
    case Color::Black:
      fillRectImpl<Color::Black>(x, y, width, height);
      break;
    case Color::White:
      fillRectImpl<Color::White>(x, y, width, height);
      break;
    case Color::LightGray:
      fillRectImpl<Color::LightGray>(x, y, width, height);
      break;
    case Color::DarkGray:
      fillRectImpl<Color::DarkGray>(x, y, width, height);
      break;
  }
}

template <Color C>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  RICKY_PAINT_SCOPE(Fill);
  if constexpr (C == Color::Clear) return;
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max({0, x, clipLeft_});
  const int ly0 = std::max({0, y, clipTop_});
  const int lx1 = std::min({screenW, x + width, clipRight_});
  const int ly1 = std::min({screenH, y + height, clipBottom_});
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // 16 级通路：矩形填充也要进 4bpp 缓冲。
  //
  // 下面那套字节级填充为了速度**直接写帧缓冲**，绕开 drawPixel()，因此不会被镜像 ——
  // 实测表现就是状态栏的文字回来了（字形走 drawPixel），而进度条整条不见（它是一块
  // 填充矩形）。这里在 16 级缓冲活跃时改走逐像素，让 drawPixel() 的镜像生效。
  //
  // 只有这一条路会变慢，而且只在 16 级那一趟；别的渲染路径仍然走下面的字节级填充。
  // / 16-level path: rectangle fills have to reach the 4bpp buffer too.
  //
  // The byte-level fill below writes the framebuffer DIRECTLY for speed and therefore
  // bypasses drawPixel(), so it is not mirrored -- on device that showed up as the status
  // bar text coming back (glyphs go through drawPixel) while the progress bar vanished
  // entirely (it is a filled rect). With that buffer active this walks pixel by pixel so
  // the mirroring applies. Only this path slows down, and only during the 16-level pass;
  // every other render path keeps the byte-level fill below.
  if (grayscale16Buffer != nullptr && !_stripActive) {
    // The 16-level frame has real grays: a smooth tone instead of the B/W dot pattern
    // (and treating these as white made light-gray tracks vanish once a screen settled).
    // Black/white write the levels drawPixel() mirrors (0 and 15).
    const uint8_t level = C == Color::LightGray                             ? kLightGrayLevel
                          : C == Color::DarkGray                            ? kDarkGrayLevel
                          : framebufferState(renderMode, C == Color::Black) ? 0
                                                                            : 15;
    fillGrayscale16Rect(lx0, ly0, lx1 - lx0, ly1 - ly0, level);
    return;
  }
  if (grayscale16Buffer != nullptr) {  // strip target: the per-pixel path clips to the band
    if constexpr (C == Color::LightGray || C == Color::DarkGray) {
      constexpr uint8_t gray = (C == Color::LightGray ? kLightGrayLevel : kDarkGrayLevel) * 17;
      for (int px = lx0; px < lx1; ++px) {
        for (int py = ly0; py < ly1; ++py) drawGrayscale16Pixel(px, py, gray);
      }
      return;
    }
    const bool black = (C == Color::Black);
    for (int py = ly0; py < ly1; ++py) {
      for (int px = lx0; px < lx1; ++px) {
        drawPixel(px, py, black);
      }
    }
    return;
  }

  // Rotate the two opposing logical corners into physical-framebuffer space.
  // The bounding rect in physical space is the rect we need to fill — rotation
  // is rigid (no shear/stretch) so the bbox of the two corners IS the rect.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  int phyY0 = std::min(paY, pbY);
  int phyY1 = std::max(paY, pbY);

  // Strip mode: clip Y range to the active band and redirect writes.
  uint8_t* target = getWriteTarget();
  const int originY = getWriteOriginY();
  const int writeRows = getWriteRows();
  phyY0 = std::max(phyY0, originY);
  phyY1 = std::min(phyY1, originY + writeRows - 1);
  if (phyY0 > phyY1) return;

  // Bit/byte layout: MSB-first within a byte, so phyX → bit (7 - (phyX & 7)).
  // Head and tail masks cover only the in-rect bits of the first/last byte.
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  if constexpr (C == Color::Black || C == Color::White) {
    // Solid fill. Framebuffer: 0 = black, 1 = white.
    const uint8_t fillByte = (C == Color::Black) ? 0x00u : 0xFFu;
    for (int py = phyY0; py <= phyY1; ++py) {
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t mask = headMask & tailMask;
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~mask);
        } else {
          row[byteStart] |= mask;
        }
      } else {
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~headMask);
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] &= static_cast<uint8_t>(~tailMask);
        } else {
          row[byteStart] |= headMask;
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] |= tailMask;
        }
      }
    }
  } else {
    // Dither (LightGray / DarkGray). Both patterns have period 2 in logical
    // (x, y), so per physical row we precompute one byte that represents the
    // pattern across an 8-pixel stretch — every full byte in the row uses
    // that same value.
    //
    // dlxPerPhyX / dlyPerPhyX: how logical (x, y) change as phyX increments
    // along a physical row. Derived from inverting rotateCoordinates.
    int dlxPerPhyX = 0, dlyPerPhyX = 0;
    switch (orientation) {
      case Portrait:
        dlxPerPhyX = 0;
        dlyPerPhyX = 1;
        break;
      case PortraitInverted:
        dlxPerPhyX = 0;
        dlyPerPhyX = -1;
        break;
      case LandscapeClockwise:
        dlxPerPhyX = -1;
        dlyPerPhyX = 0;
        break;
      case LandscapeCounterClockwise:
        dlxPerPhyX = 1;
        dlyPerPhyX = 0;
        break;
    }

    // The dither pattern has period 2 in logical space, and each orientation
    // maps py to logical coords with a fixed parity relationship. The
    // blackMask byte therefore repeats with period 2 in py. Precompute both
    // variants outside the row loop to eliminate the per-row switch + 8-bit
    // construction loop.
    uint8_t blackMasks[2];
    for (int parityIdx = 0; parityIdx < 2; ++parityIdx) {
      const int samplePy = phyY0 + parityIdx;
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - samplePy;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = samplePy;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - samplePy;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = samplePy;
          break;
      }
      uint8_t mask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else {  // DarkGray
          isBlack = (((lx + ly) & 1) == 0);
        }
        if (isBlack) mask |= static_cast<uint8_t>(1u << (7 - b));
      }
      blackMasks[samplePy & 1] = mask;
    }

    for (int py = phyY0; py <= phyY1; ++py) {
      const uint8_t blackMask = blackMasks[py & 1];
      const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // Dither writes BOTH inks (the slow path called drawPixel for every
      // pixel — setting or clearing — so we must do the same). Inside the
      // rect mask: write whiteMask (1s where white, 0s where black). Outside
      // the rect mask: leave the framebuffer untouched.
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t rectMask = headMask & tailMask;
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~rectMask) | (rectMask & whiteMask));
      } else {
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (headMask & whiteMask));
        if (byteEnd > byteStart + 1) {
          // Period 2, so every full byte in this row is exactly whiteMask.
          memset(row + byteStart + 1, whiteMask, byteEnd - byteStart - 1);
        }
        row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (tailMask & whiteMask));
      }
    }
  }
}

template void GfxRenderer::fillRectImpl<Color::Black>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::White>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::LightGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::DarkGray>(int, int, int, int) const;

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;
  if constexpr (color == Color::Black || color == Color::White) {
    if (grayscale16Buffer) {
      antialiasedQuarter(maxRadius, cx, cy, xDir, yDir, 0, framebufferState(renderMode, color == Color::Black));
      return;
    }
  }

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  RICKY_PAINT_SCOPE(Fill);
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  RICKY_PAINT_SCOPE(Image);
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int size) const {
  RICKY_PAINT_SCOPE(Image);
  // Plot the icon pixel-by-pixel through drawPixel (which applies the orientation
  // transform) instead of the byte-aligned framebuffer blit. The blit snaps the
  // icon's position to 8px (one byte) along the rotated axis, which prevents it
  // from aligning with adjacent text; per-pixel plotting is pixel-precise.
  // Icons are square and 1bpp (MSB-first, bit==0 = ink). The (size-1-row, col)
  // mapping reproduces the Portrait orientation the blit produced; drawIcon is
  // only called by the UI themes, which all render in forced Portrait.
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        drawPixel(x + (size - 1 - row), y + col, true);
      }
    }
  }
}

void GfxRenderer::drawIconInverted(const uint8_t bitmap[], const int x, const int y, const int size) const {
  RICKY_PAINT_SCOPE(Image);
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        drawPixel(x + (size - 1 - row), y + col, false);
      }
    }
  }
}

bool GfxRenderer::drawBitmapCropToFill(const Bitmap& bitmap, const int x, const int y, const int targetWidth,
                                       const int targetHeight) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return false;
  const int sourceWidth = bitmap.getWidth();
  const int sourceHeight = bitmap.getHeight();
  if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) return false;

  int scaledWidth = targetWidth;
  int scaledHeight =
      static_cast<int>((static_cast<int64_t>(sourceHeight) * targetWidth + sourceWidth - 1) / sourceWidth);
  if (scaledHeight < targetHeight) {
    scaledHeight = targetHeight;
    scaledWidth =
        static_cast<int>((static_cast<int64_t>(sourceWidth) * targetHeight + sourceHeight - 1) / sourceHeight);
  }
  const int cropLeft = (scaledWidth - targetWidth) / 2;
  const int cropTop = (scaledHeight - targetHeight) / 2;

  // Same bounded two-row working set as drawBitmap(); a scaled image buffer would exceed the RAM budget.
  const int outputRowSize = (sourceWidth + 3) / 4;
  const size_t scratchSize = static_cast<size_t>(outputRowSize) + bitmap.getRowBytes();
  if (!bitmap.ensureDrawScratch(scratchSize)) {
    LOG_ERR("GFX", "Failed to allocate crop-fill row buffers (%u bytes)", static_cast<unsigned>(scratchSize));
    return false;
  }
  uint8_t* const outputRow = bitmap.drawScratch.get();
  uint8_t* const rowBytes = outputRow + outputRowSize;

  const GfxRenderer::RenderMode mode = getRenderMode();
  const auto runPixel = mapTwoBitPixel(mode, 0);
  for (int sourceRow = 0; sourceRow < sourceHeight; ++sourceRow) {
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read crop-fill row %d", sourceRow);
      return false;
    }

    const int logicalRow = bitmap.isTopDown() ? sourceRow : sourceHeight - 1 - sourceRow;
    const int rowTop = logicalRow * scaledHeight / sourceHeight - cropTop;
    const int rowBottom = (logicalRow + 1) * scaledHeight / sourceHeight - cropTop;
    const int clippedTop = std::max(0, rowTop);
    const int clippedBottom = std::min(targetHeight, rowBottom);
    if (clippedTop >= clippedBottom) continue;

    int runStart = -1;
    for (int sourceX = 0; sourceX <= sourceWidth; ++sourceX) {
      bool draw = false;
      if (sourceX < sourceWidth) {
        const uint8_t value = outputRow[sourceX / 4] >> (6 - ((sourceX * 2) % 8)) & 0x3;
        draw = mapTwoBitPixel(mode, value).draw;
      }
      if (draw && runStart < 0) {
        runStart = sourceX;
      } else if (!draw && runStart >= 0) {
        const int runLeft = runStart * scaledWidth / sourceWidth - cropLeft;
        const int runRight = sourceX * scaledWidth / sourceWidth - cropLeft;
        const int clippedLeft = std::max(0, runLeft);
        const int clippedRight = std::min(targetWidth, runRight);
        if (clippedLeft < clippedRight) {
          fillRect(x + clippedLeft, y + clippedTop, clippedRight - clippedLeft, clippedBottom - clippedTop,
                   runPixel.state);
        }
        runStart = -1;
      }
    }
  }
  return true;
}

uint8_t GfxRenderer::getGrayscaleLevels() const { return grayscaleLevels(display); }

namespace {
template <typename Display>
void configureGrayUi(Display& target, int x, int y, int w, int h, uint8_t contentEvery, uint8_t smallEvery, bool adaptive) {
  if constexpr (requires { target.setNextGray16UiRefresh(x, y, w, h, contentEvery, smallEvery, adaptive); }) {
    target.setNextGray16UiRefresh(x, y, w, h, contentEvery, smallEvery, adaptive);
  } else if constexpr (requires { target.setNextGray16UiRefresh(x, y, w, h, contentEvery, smallEvery); }) {
    target.setNextGray16UiRefresh(x, y, w, h, contentEvery, smallEvery);
  }
}
template <typename Display>
void configureMainTabTurn(Display& target, uint8_t orientation, bool right, int x, int y, int w, int h) {
  if constexpr (requires { target.setNextMainTabTurn(orientation, right, x, y, w, h); }) {
    target.setNextMainTabTurn(orientation, right, x, y, w, h);
  }
}
template <typename Display>
void configureGrayReader(Display& target, int x, int y, int w, int h) {
  if constexpr (requires { target.setNextGray16ReaderRefresh(x, y, w, h); }) {
    target.setNextGray16ReaderRefresh(x, y, w, h);
  }
}
}  // namespace

void GfxRenderer::mapGrayRefreshArea(int& x, int& y, int& width, int& height) const {
  const int x0 = std::clamp(x, 0, getScreenWidth());
  const int y0 = std::clamp(y, 0, getScreenHeight());
  const int x1 = std::clamp(x + std::max(0, width), 0, getScreenWidth());
  const int y1 = std::clamp(y + std::max(0, height), 0, getScreenHeight());
  x = x0;
  y = y0;
  width = std::max(0, x1 - x0);
  height = std::max(0, y1 - y0);
  switch (orientation) {
    case Portrait:
      x = y0;
      y = panelHeight - x1;
      std::swap(width, height);
      break;
    case PortraitInverted:
      x = panelWidth - y1;
      y = x0;
      std::swap(width, height);
      break;
    case LandscapeClockwise:
      x = panelWidth - x1;
      y = panelHeight - y1;
      break;
    case LandscapeCounterClockwise:
      break;
  }
}

void GfxRenderer::setNextGray16UiRefresh(int x, int y, int width, int height, const uint8_t contentEvery,
                                       const uint8_t smallEvery, bool adaptive) const {
  mapGrayRefreshArea(x, y, width, height);
  configureGrayUi(display, x, y, width, height, contentEvery, smallEvery, adaptive);
}

void GfxRenderer::setNextMainTabTurn(int x, int y, int width, int height, bool fromRight) const {
  mapGrayRefreshArea(x, y, width, height);
  configureMainTabTurn(display, static_cast<uint8_t>(orientation), fromRight, x, y, width, height);
}

void GfxRenderer::setNextGray16ReaderRefresh(int x, int y, int width, int height) const {
  mapGrayRefreshArea(x, y, width, height);
  configureGrayReader(display, x, y, width, height);
}

bool GfxRenderer::beginGrayscale16() {
  if (grayscale16Buffer || _stripActive || !frameBuffer || getGrayscaleLevels() != 16) return false;
  grayscale16Buffer = beginNativeGray(display);
  if (!grayscale16Buffer) return false;
  setRenderMode(BW);
  clearScreen();  // Retain a B/W proxy for popup drawing and subsequent AA.
  return true;
}

bool GfxRenderer::beginGrayscale16Offscreen(uint8_t* frame) {
  if (!frame || grayscale16Buffer || _stripActive || !frameBuffer || getGrayscaleLevels() != 16) return false;
  memset(frame, 0xFF, grayscale16FrameBytes());  // level 15 = white, as the native begin clears
  grayscale16Buffer = frame;
  setRenderMode(BW);
  // clearScreen() reaches the panel HAL on Read Pico; the framebuffer is scratch here.
  memset(frameBuffer, 0xFF, getBufferSize());
  return true;
}

bool GfxRenderer::commitGrayscale16() const {
  if (!grayscale16Buffer) return false;
#if FREEINK_DEVICE_METALIO_PLUS
  // Standby/overlay exit requests a clean baseline. Native16 must honor the
  // same one-shot request as displayBuffer(), without consuming it on failure.
  const bool hadRefreshOverride = nextRefreshOverridePending;
  if (nextRefreshNeedsCleanup()) display.setNextGray16Profile(2);
#endif
  const bool committed = commitNativeGray(display);
  if (!committed) cancelNativeGray(display);
#if FREEINK_DEVICE_METALIO_PLUS
  if (committed && hadRefreshOverride) clearNextRefreshOverride();
#endif
  grayscale16Buffer = nullptr;
  return committed;
}

bool GfxRenderer::beginRetainedGrayscale16() {
  if (grayscale16Buffer || _stripActive || !frameBuffer || getGrayscaleLevels() != 16) return false;
  grayscale16Buffer = beginRetainedNativeGray(display);
  if (!grayscale16Buffer) return false;
  setRenderMode(BW);
  return true;  // Native frame and B/W proxy both match the displayed baseline.
}

void GfxRenderer::cancelGrayscale16() const {
  if (grayscale16Buffer) cancelNativeGray(display);
  grayscale16Buffer = nullptr;
}

// One quarter of a rounded corner straight into the 16-level frame, with edge coverage
// from a 4x4 sample grid blended over what is already there: the 1-bit quarter-circle
// steps are what made rounded cards and pills look coarse at 300 ppi. Same geometry as
// the 1-bit arcs (pixel centres within the radius), so layouts do not shift. stroke 0
// fills the quarter disc.
void GfxRenderer::antialiasedQuarter(const int radius, const int cx, const int cy, const int xDir, const int yDir,
                                     const int stroke, const bool ink) const {
  const float outer = static_cast<float>(radius) + 0.5f;
  const float inner = stroke > 0 ? std::max(0.0f, static_cast<float>(radius - stroke) + 0.5f) : -1.0f;
  const float outerSq = outer * outer;
  const float innerSq = inner * inner;
  const int inkLevel = ink ? 0 : 15;
  for (int dx = 0; dx <= radius; ++dx) {  // columns: contiguous in panel memory in portrait
    for (int dy = 0; dy <= radius; ++dy) {
      int covered = 0;
      for (int i = 0; i < 4; ++i) {
        const float sx = static_cast<float>(dx) - 0.375f + 0.25f * static_cast<float>(i);
        for (int j = 0; j < 4; ++j) {
          const float sy = static_cast<float>(dy) - 0.375f + 0.25f * static_cast<float>(j);
          const float d = sx * sx + sy * sy;
          if (d <= outerSq && (inner < 0 || d >= innerSq)) ++covered;
        }
      }
      if (covered == 0) continue;
      const int x = cx + xDir * dx;
      const int y = cy + yDir * dy;
      const int paper = grayscale16Level(x, y);
      const int delta = (inkLevel - paper) * covered;  // over 16 samples, rounded either way
      const int level = paper + (delta >= 0 ? (delta + 8) / 16 : -((8 - delta) / 16));
      drawGrayscale16Pixel(x, y, static_cast<uint8_t>(level * 17));
    }
  }
}

uint8_t GfxRenderer::grayscale16Level(const int x, const int y) const {
  if (!grayscale16Buffer) return 15;
  int px, py;
  rotateCoordinates(orientation, x, y, &px, &py, panelWidth, panelHeight);
  if (px < 0 || px >= panelWidth || py < 0 || py >= panelHeight) return 15;
  const size_t index = static_cast<size_t>(py) * (panelWidth / 2) + px / 2;
  return (grayscale16Buffer[index] >> ((px & 1) * 4)) & 0x0F;
}

void GfxRenderer::blendGrayscale16Pixel(const int x, const int y, const uint8_t inkLevel,
                                        const uint8_t coverage) const {
  if (!grayscale16Buffer || !coverage || x < clipLeft_ || y < clipTop_ || x >= clipRight_ || y >= clipBottom_) return;
  int px, py;
  rotateCoordinates(orientation, x, y, &px, &py, panelWidth, panelHeight);
  if (px < 0 || px >= panelWidth || py < 0 || py >= panelHeight) return;
  const int ink = std::min<uint8_t>(inkLevel, 15);
  const int alpha = std::min<uint8_t>(coverage, 15);
  if (alpha == 15) {
    writeGrayscale16Physical(px, py, static_cast<uint8_t>(ink));
    return;
  }
  const size_t index = static_cast<size_t>(py) * (panelWidth / 2) + px / 2;
  const int paper = (grayscale16Buffer[index] >> ((px & 1) * 4)) & 15;
  const int delta = (ink - paper) * alpha;
  const int level = paper + (delta >= 0 ? (delta + 7) / 15 : -((7 - delta) / 15));
  writeGrayscale16Physical(px, py, static_cast<uint8_t>(level));
}

void GfxRenderer::drawGrayscale16Pixel(const int x, const int y, const uint8_t gray) const {
  // Hot path of every anti-aliased text page: one clip test, one rotation, then the
  // B/W proxy bit and the 4-bit level written directly. Going through drawPixel()
  // (which also mirrors into this buffer) cost three rotations and two nibble writes
  // per glyph pixel and made the single 16-level render slower than three 1-bit passes.
  if (!grayscale16Buffer || x < clipLeft_ || y < clipTop_ || x >= clipRight_ || y >= clipBottom_) return;
  int px, py;
  rotateCoordinates(orientation, x, y, &px, &py, panelWidth, panelHeight);
  if (px < 0 || px >= panelWidth || py < 0 || py >= panelHeight) return;
  const uint8_t level = (static_cast<unsigned>(gray) + 8) / 17;
  writeGrayscale16Physical(px, py, level);
}

void GfxRenderer::fillGrayscale16Rect(const int x, const int y, const int width, const int height,
                                      const uint8_t level) const {
  RICKY_PAINT_SCOPE(Fill);
  if (!grayscale16Buffer || width <= 0 || height <= 0 || level > 15) return;
  const int lx0 = std::max({0, x, clipLeft_});
  const int ly0 = std::max({0, y, clipTop_});
  const int lx1 = std::min({getScreenWidth(), x + width, clipRight_});
  const int ly1 = std::min({getScreenHeight(), y + height, clipBottom_});
  if (lx0 >= lx1 || ly0 >= ly1) return;
  // Rotation is rigid, so the two opposite corners bound the physical rectangle.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, lx0, ly0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &bx, &by, panelWidth, panelHeight);
  const int px0 = std::max(0, std::min(ax, bx));
  const int px1 = std::min(static_cast<int>(panelWidth) - 1, std::max(ax, bx));
  const int py0 = std::max(0, std::min(ay, by));
  const int py1 = std::min(static_cast<int>(panelHeight) - 1, std::max(ay, by));
  if (px0 > px1 || py0 > py1) return;

  const bool ink = framebufferState(renderMode, level < 8);
  const uint8_t pair = static_cast<uint8_t>(level | (level << 4));
  const size_t grayStride = panelWidth / 2;
  // B/W proxy: MSB-first bits, ink clears a bit. Partial edge bytes are masked.
  const int firstByte = px0 / 8;
  const int lastByte = px1 / 8;
  const uint8_t firstMask = static_cast<uint8_t>(0xFFu >> (px0 % 8));
  const uint8_t lastMask = static_cast<uint8_t>(0xFFu << (7 - px1 % 8));
  for (int py = py0; py <= py1; ++py) {
    uint8_t* gray = grayscale16Buffer + static_cast<size_t>(py) * grayStride;
    int a = px0;
    int b = px1;
    if (a & 1) {  // odd x is the high nibble of its byte
      gray[a / 2] = static_cast<uint8_t>((gray[a / 2] & 0x0F) | (level << 4));
      ++a;
    }
    if (a <= b && !(b & 1)) {  // even x is the low nibble
      gray[b / 2] = static_cast<uint8_t>((gray[b / 2] & 0xF0) | level);
      --b;
    }
    if (a <= b) memset(gray + a / 2, pair, static_cast<size_t>(b - a + 1) / 2);

    uint8_t* bw = frameBuffer + static_cast<size_t>(py) * panelWidthBytes;
    const auto apply = [ink](uint8_t& byte, const uint8_t mask) {
      byte = ink ? static_cast<uint8_t>(byte & ~mask) : static_cast<uint8_t>(byte | mask);
    };
    if (firstByte == lastByte) {
      apply(bw[firstByte], static_cast<uint8_t>(firstMask & lastMask));
    } else {
      apply(bw[firstByte], firstMask);
      if (lastByte - firstByte > 1)
        memset(bw + firstByte + 1, ink ? 0x00 : 0xFF, static_cast<size_t>(lastByte - firstByte - 1));
      apply(bw[lastByte], lastMask);
    }
  }
}

size_t GfxRenderer::PanelTile::bytes() const {
  const size_t rows = static_cast<size_t>(py1 - py0 + 1);
  return rows * static_cast<size_t>(px1 / 2 - px0 / 2 + 1 + px1 / 8 - px0 / 8 + 1);
}

bool GfxRenderer::panelTileFor(const int x, const int y, const int width, const int height, PanelTile* tile) const {
  if (!grayscale16Buffer || !frameBuffer || _stripActive || width <= 0 || height <= 0) return false;
  const int lx0 = std::max({0, x, clipLeft_});
  const int ly0 = std::max({0, y, clipTop_});
  const int lx1 = std::min({getScreenWidth(), x + width, clipRight_});
  const int ly1 = std::min({getScreenHeight(), y + height, clipBottom_});
  if (lx0 >= lx1 || ly0 >= ly1) return false;
  int ax, ay, bx, by;
  rotateCoordinates(orientation, lx0, ly0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &bx, &by, panelWidth, panelHeight);
  tile->lx0 = static_cast<int16_t>(lx0);
  tile->ly0 = static_cast<int16_t>(ly0);
  tile->lx1 = static_cast<int16_t>(lx1);
  tile->ly1 = static_cast<int16_t>(ly1);
  tile->px0 = static_cast<int16_t>(std::max(0, std::min(ax, bx)));
  tile->px1 = static_cast<int16_t>(std::min(static_cast<int>(panelWidth) - 1, std::max(ax, bx)));
  tile->py0 = static_cast<int16_t>(std::max(0, std::min(ay, by)));
  tile->py1 = static_cast<int16_t>(std::min(static_cast<int>(panelHeight) - 1, std::max(ay, by)));
  tile->orientation = static_cast<uint8_t>(orientation);
  return tile->px0 <= tile->px1 && tile->py0 <= tile->py1;
}

void GfxRenderer::capturePanelTile(const PanelTile& tile, uint8_t* out) const {
  const size_t grayBytes = static_cast<size_t>(tile.px1 / 2 - tile.px0 / 2 + 1);
  const size_t bwBytes = static_cast<size_t>(tile.px1 / 8 - tile.px0 / 8 + 1);
  for (int py = tile.py0; py <= tile.py1; ++py) {
    memcpy(out, grayscale16Buffer + static_cast<size_t>(py) * (panelWidth / 2) + tile.px0 / 2, grayBytes);
    out += grayBytes;
    memcpy(out, frameBuffer + static_cast<size_t>(py) * panelWidthBytes + tile.px0 / 8, bwBytes);
    out += bwBytes;
  }
}

bool GfxRenderer::restorePanelTile(const PanelTile& tile, const uint8_t* in) const {
  RICKY_PAINT_SCOPE(Image);
  PanelTile now;
  if (!panelTileFor(tile.lx0, tile.ly0, tile.lx1 - tile.lx0, tile.ly1 - tile.ly0, &now) ||
      now.lx0 != tile.lx0 || now.ly0 != tile.ly0 || now.lx1 != tile.lx1 || now.ly1 != tile.ly1 ||
      now.px0 != tile.px0 || now.py0 != tile.py0 || now.px1 != tile.px1 || now.py1 != tile.py1 ||
      now.orientation != tile.orientation)
    return false;
  const int g0 = tile.px0 / 2, g1 = tile.px1 / 2;
  const int b0 = tile.px0 / 8, b1 = tile.px1 / 8;
  // Edge bytes are shared with neighbours: odd x is the high nibble, B/W is MSB first.
  const uint8_t grayFirst = (tile.px0 & 1) ? 0xF0 : 0xFF;
  const uint8_t grayLast = (tile.px1 & 1) ? 0xFF : 0x0F;
  const uint8_t bwFirst = static_cast<uint8_t>(0xFFu >> (tile.px0 % 8));
  const uint8_t bwLast = static_cast<uint8_t>(0xFFu << (7 - tile.px1 % 8));
  const auto copyRow = [](uint8_t* dst, const uint8_t* src, const int count, const uint8_t first,
                          const uint8_t last) {
    const auto merge = [](uint8_t& byte, const uint8_t value, const uint8_t mask) {
      byte = static_cast<uint8_t>((byte & ~mask) | (value & mask));
    };
    if (count == 1) {
      merge(dst[0], src[0], static_cast<uint8_t>(first & last));
      return;
    }
    merge(dst[0], src[0], first);
    if (count > 2) memcpy(dst + 1, src + 1, static_cast<size_t>(count - 2));
    merge(dst[count - 1], src[count - 1], last);
  };
  for (int py = tile.py0; py <= tile.py1; ++py) {
    copyRow(grayscale16Buffer + static_cast<size_t>(py) * (panelWidth / 2) + g0, in, g1 - g0 + 1, grayFirst,
            grayLast);
    in += g1 - g0 + 1;
    copyRow(frameBuffer + static_cast<size_t>(py) * panelWidthBytes + b0, in, b1 - b0 + 1, bwFirst, bwLast);
    in += b1 - b0 + 1;
  }
  return true;
}

void GfxRenderer::writeGrayscale16Physical(const int px, const int py, const uint8_t level) const {
  // B/W proxy for popups and later partial updates: level < 8 is ink, as drawPixel(x, y,
  // level < 8) would write it.
  const uint32_t byteIndex = static_cast<uint32_t>(py) * panelWidthBytes + (px / 8);
  const uint8_t bit = 1u << (7 - (px % 8));
  if (framebufferState(renderMode, level < 8)) {
    frameBuffer[byteIndex] &= ~bit;
  } else {
    frameBuffer[byteIndex] |= bit;
  }
  const size_t index = static_cast<size_t>(py) * (panelWidth / 2) + px / 2;
  const unsigned shift = (px & 1) * 4;
  grayscale16Buffer[index] = (grayscale16Buffer[index] & ~(0x0Fu << shift)) | (level << shift);
}

void GfxRenderer::copyBwToGrayscale16(const int x, const int y, const int width, const int height,
                                      const int cornerRadius) const {
  if (!grayscale16Buffer || _stripActive || !frameBuffer) return;
  const int radius = std::max(0, std::min(cornerRadius, std::min(width, height) / 2));
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      if (radius > 0) {
        // Distance into the nearest corner square; outside the quarter circle stays as is.
        const int dx =
            column < radius ? radius - column : (column >= width - radius ? column - (width - radius - 1) : 0);
        const int dy = row < radius ? radius - row : (row >= height - radius ? row - (height - radius - 1) : 0);
        if (dx > 0 && dy > 0 && dx * dx + dy * dy > radius * radius) continue;
      }
      const int sx = x + column;
      const int sy = y + row;
      if (sx < 0 || sy < 0 || sx >= getScreenWidth() || sy >= getScreenHeight()) continue;
      int px, py;
      rotateCoordinates(orientation, sx, sy, &px, &py, panelWidth, panelHeight);
      const bool ink = (frameBuffer[static_cast<size_t>(py) * panelWidthBytes + px / 8] & (0x80U >> (px % 8))) == 0;
      const size_t index = static_cast<size_t>(py) * (panelWidth / 2) + px / 2;
      const unsigned shift = (px & 1) * 4;
      const uint8_t level = ink ? 0 : 15;
      grayscale16Buffer[index] = (grayscale16Buffer[index] & ~(0x0Fu << shift)) | (level << shift);
    }
  }
}

bool GfxRenderer::drawBitmapGrayscale16(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                        const int maxHeight, const float cropX, const float cropY) const {
  if (!grayscale16Buffer || !std::isfinite(cropX) || !std::isfinite(cropY) || cropX < 0 || cropX >= 1 || cropY < 0 ||
      cropY >= 1 || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0)
    return false;
  const int width = bitmap.getWidth(), height = bitmap.getHeight();
  const int cropPixX = std::floor(width * cropX / 2), cropPixY = std::floor(height * cropY / 2);
  float scale = 1;
  if (maxWidth > 0) scale = std::min(scale, maxWidth / ((1 - cropX) * width));
  if (maxHeight > 0) scale = std::min(scale, maxHeight / ((1 - cropY) * height));
  // At most 2048 Gray8 pixels + 8192 source bytes, bounded by Bitmap headers.
  if (!bitmap.ensureDrawScratch(static_cast<size_t>(width) + bitmap.getRowBytes())) {
    LOG_ERR("GFX", "Failed to allocate native BMP rows");
    return false;
  }
  const int sourceWidth = width - cropPixX * 2, sourceHeight = height - cropPixY * 2;
  const int targetWidth = std::floor((sourceWidth - 1) * scale) + 1;
  const int targetHeight = std::floor((sourceHeight - 1) * scale) + 1;
  uint8_t* row = bitmap.drawScratch.get();
  uint8_t* source = row + width;
  // In portrait, writing a logical row strides a whole panel row per pixel (a PSRAM cache
  // miss each): ~2.3 s for a full-screen picture. Collect a band of rows and write it
  // column by column, where consecutive pixels are adjacent in panel memory.
  constexpr int kBandRows = 32;
  std::unique_ptr<uint8_t[]> band;
  if (grayscale16PrefersColumns()) band.reset(new (std::nothrow) uint8_t[static_cast<size_t>(targetWidth) * kBandRows]);
  int bandDestY[kBandRows];
  int bandCount = 0;
  const auto flushBand = [&] {
    for (int destX = 0; destX < targetWidth; ++destX) {
      for (int i = 0; i < bandCount; ++i) {
        drawGrayscale16Pixel(x + destX, y + bandDestY[i], band[static_cast<size_t>(i) * targetWidth + destX]);
      }
    }
    bandCount = 0;
  };
  for (int fileY = 0; fileY < height; ++fileY) {
    if (bitmap.readNextRow(row, source, nullptr, Bitmap::RowOutput::Gray8) != BmpReaderError::Ok) return false;
    const int sourceY = bitmap.isTopDown() ? fileY : height - 1 - fileY;
    if (sourceY < cropPixY || sourceY >= height - cropPixY) continue;
    const int relativeY = sourceY - cropPixY;
    const int destY = (relativeY * targetHeight + sourceHeight - 1) / sourceHeight;
    // Select one source row per destination row, independent of BMP row order.
    if (destY >= targetHeight || destY * sourceHeight / targetHeight != relativeY) continue;
    if (band) {
      uint8_t* out = &band[static_cast<size_t>(bandCount) * targetWidth];
      for (int destX = 0; destX < targetWidth; ++destX) out[destX] = row[cropPixX + destX * sourceWidth / targetWidth];
      bandDestY[bandCount++] = destY;
      if (bandCount == kBandRows) flushBand();
      continue;
    }
    for (int destX = 0; destX < targetWidth; ++destX) {
      const int sourceX = cropPixX + destX * sourceWidth / targetWidth;
      drawGrayscale16Pixel(x + destX, y + destY, row[sourceX]);
    }
  }
  if (band && bandCount > 0) flushBand();
  return true;
}

bool GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY, const bool preserveTransparency) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return false;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    return drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  const bool useTransparency = preserveTransparency && bitmap.hasTransparency();
  const size_t scratchSize = static_cast<size_t>(outputRowSize) + bitmap.getRowBytes() +
                             (useTransparency ? static_cast<size_t>(bitmap.getWidth()) : 0);
  if (!bitmap.ensureDrawScratch(scratchSize)) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    return false;
  }
  uint8_t* const outputRow = bitmap.drawScratch.get();
  uint8_t* const rowBytes = outputRow + outputRowSize;
  uint8_t* const opacityRow = useTransparency ? rowBytes + bitmap.getRowBytes() : nullptr;

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes, opacityRow) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);

      return false;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (useTransparency && !opacityRow[bmpX]) {
        continue;
      }
      auto pixel = mapTwoBitPixel(renderMode, val);
      if (renderMode == BW && useTransparency && val >= 3) pixel = {true, false};
      if (pixel.draw) drawPixel(screenX, screenY, pixel.state);
    }
  }

  const int sourceWidth = bitmap.getWidth() - cropPixX * 2;
  const int sourceHeight = bitmap.getHeight() - cropPixY * 2;
  const int renderedWidth = isScaled ? static_cast<int>(std::floor((sourceWidth - 1) * scale)) + 1 : sourceWidth;
  const int renderedHeight = isScaled ? static_cast<int>(std::floor((sourceHeight - 1) * scale)) + 1 : sourceHeight;
  preserveImagePolarity(x, y, renderedWidth, renderedHeight);
  return true;
}

bool GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  const size_t scratchSize = static_cast<size_t>(outputRowSize) + bitmap.getRowBytes();
  if (!bitmap.ensureDrawScratch(scratchSize)) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    return false;
  }
  uint8_t* const outputRow = bitmap.drawScratch.get();
  uint8_t* const rowBytes = outputRow + outputRowSize;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);

      return false;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

  const int renderedWidth =
      isScaled ? static_cast<int>(std::floor((bitmap.getWidth() - 1) * scale)) + 1 : bitmap.getWidth();
  const int renderedHeight =
      isScaled ? static_cast<int>(std::floor((bitmap.getHeight() - 1) * scale)) + 1 : bitmap.getHeight();
  preserveImagePolarity(x, y, renderedWidth, renderedHeight);
  return true;
}

void GfxRenderer::preserveImagePolarity(const int x, const int y, const int width, const int height) const {
  if (renderMode != BW || !display.isInverted() || _stripActive || !frameBuffer || width <= 0 || height <= 0) return;

  const int lx0 = std::max(x, clipLeft_);
  const int ly0 = std::max(y, clipTop_);
  const int lx1 = std::min(x + width, clipRight_);
  const int ly1 = std::min(y + height, clipBottom_);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  int ax, ay, bx, by;
  rotateCoordinates(orientation, lx0, ly0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &bx, &by, panelWidth, panelHeight);

  int left = std::max(0, std::min(ax, bx));
  int right = std::min(static_cast<int>(panelWidth) - 1, std::max(ax, bx));
  int top = std::max(0, std::min(ay, by));
  int bottom = std::min(static_cast<int>(panelHeight) - 1, std::max(ay, by));
  if (left > right || top > bottom) return;

  for (int row = top; row <= bottom; row++) {
    uint8_t* rowData = frameBuffer + static_cast<uint32_t>(row) * panelWidthBytes;
    int col = left;
    while (col <= right && (col & 7) != 0) {
      rowData[col >> 3] ^= static_cast<uint8_t>(0x80U >> (col & 7));
      col++;
    }
    while (col + 7 <= right) {
      rowData[col >> 3] ^= 0xFF;
      col += 8;
    }
    while (col <= right) {
      rowData[col >> 3] ^= static_cast<uint8_t>(0x80U >> (col & 7));
      col++;
    }
  }
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto nodeX = makeUniqueNoThrow<int[]>(numPoints);
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX.get(), nodeX.get() + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  if (_stripActive) {
    // Clear only the active band's scratch, not the shared framebuffer.
    memset(_stripBuf, color, static_cast<size_t>(panelWidthBytes) * _stripRows);
    return;
  }
  display.clearScreen(color);
  if (displayCapture_ && grayscale16Buffer) {
    // A screen may redraw several times in one render; earlier passes must not show.
    memset(grayscale16Buffer, color == 0x00 ? 0x00 : 0xFF, grayscale16FrameBytes());
  }
}

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  // Band is caller-guaranteed in-bounds (the reader's grayscale loop computes
  // it); assert catches future misuse in debug before it mis-renders or wraps
  // the downstream uint16_t cast in writeGrayscalePlaneStrip.
  assert(scratch != nullptr && stripRows > 0 && stripY0 >= 0 && stripY0 <= static_cast<int>(panelHeight) - stripRows);
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}

void GfxRenderer::endStripTarget() const {
  _stripActive = false;
  _stripBuf = nullptr;
  _stripY0 = 0;
  _stripRows = 0;
}

bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  if (!_stripActive) {
    return true;
  }
  // Rotate the two opposite bbox corners to physical coords. For 90-degree
  // orientations the physical bbox stays axis-aligned, so min/max of the two
  // rotated corners' Y bounds the glyph's physical y-extent.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x0, y0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &bx, &by, panelWidth, panelHeight);
  const int minY = ay < by ? ay : by;
  const int maxY = ay > by ? ay : by;
  return !(maxY < _stripY0 || minY >= _stripY0 + _stripRows);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(HalDisplay::RefreshMode refreshMode, DisplayRefreshContext context) const {
  if (displayCapture_) return;  // the capture's owner commits the 16-level frame
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  HalDisplay::RefreshMode effectiveRefreshMode = refreshMode;
  if (nextRefreshOverridePending) {
    effectiveRefreshMode = nextRefreshOverride;
    nextRefreshOverridePending = false;
  }
#ifdef SIMULATOR
  (void)context;
  display.displayBuffer(effectiveRefreshMode, fadingFix);
#else
  display.displayBuffer(effectiveRefreshMode, fadingFix, context);
#endif
}

void GfxRenderer::displayBufferAsync(const HalDisplay::RefreshMode refreshMode, DisplayRefreshContext context) const {
  if (displayCapture_) return;
  HalDisplay::RefreshMode effectiveRefreshMode = refreshMode;
  if (nextRefreshOverridePending) {
    effectiveRefreshMode = nextRefreshOverride;
    nextRefreshOverridePending = false;
  }
  // The async path has no turn-off-screen hook, which the sunlight fading fix
  // relies on; keep those users on the blocking path.
  if (fadingFix) {
#ifdef SIMULATOR
    (void)context;
    display.displayBuffer(effectiveRefreshMode, fadingFix);
#else
    display.displayBuffer(effectiveRefreshMode, fadingFix, context);
#endif
    return;
  }
#ifdef SIMULATOR
  (void)context;
  display.displayBufferAsync(effectiveRefreshMode);
#else
  display.displayBufferAsync(effectiveRefreshMode, context);
#endif
}

void GfxRenderer::waitRefreshComplete() const { display.waitRefreshComplete(); }

bool GfxRenderer::supportsAsyncRefresh() const { return !fadingFix && display.supportsAsyncRefresh(); }

bool GfxRenderer::isInverted() const { return display.isInverted(); }
HalDisplay::GrayscaleCapabilities GfxRenderer::grayscaleCapabilities(HalDisplay::GrayscaleMode mode) const {
  auto caps = display.grayscaleCapabilities(mode);
  if (fadingFix) caps.asyncBase = false;
  return caps;
}

bool GfxRenderer::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

size_t GfxRenderer::readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const {
  if (dst == nullptr || w <= 0 || h <= 0) return 0;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return 0;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8
  const size_t needed = rowBytes * mem.h;
  if (needed > dstCapacity) return 0;

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    uint8_t* dstRow = dst + (static_cast<size_t>(row) * rowBytes);
    memcpy(dstRow, srcRow, rowBytes);
  }
  return needed;
}

void GfxRenderer::writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src) {
  if (src == nullptr || w <= 0 || h <= 0) return;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = src + (static_cast<size_t>(row) * rowBytes);
    uint8_t* dstRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    memcpy(dstRow, srcRow, rowBytes);
  }
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  size_t charCount = 0;
  for (const unsigned char c : item) {
    if ((c & 0xC0) != 0x80) ++charCount;
  }

  std::string candidate;
  candidate.reserve(item.size() + 3);
  const auto setCandidate = [&](const size_t characterCount) {
    size_t end = 0;
    for (size_t count = 0; end < item.size() && count < characterCount; ++count) {
      ++end;
      while (end < item.size() && (static_cast<unsigned char>(item[end]) & 0xC0) == 0x80) ++end;
    }
    candidate.assign(item.data(), end);
    candidate += ellipsis;
  };

  // Binary search avoids repeatedly measuring nearly the entire long title.
  size_t low = 0;
  size_t high = charCount;
  while (low < high) {
    const size_t mid = low + (high - low + 1) / 2;
    setCandidate(mid);
    if (getTextWidth(fontId, candidate.c_str(), style) < maxWidth) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }

  setCandidate(low);
  return candidate;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  lines.reserve(static_cast<size_t>(maxLines));
  const size_t textLength = strlen(text);
  std::string_view remaining{text, textLength};
  std::string currentLine;
  std::string candidate;
  currentLine.reserve(textLength);
  candidate.reserve(textLength);

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      candidate = currentLine;
      if (!candidate.empty() && !remaining.empty()) candidate.push_back(' ');
      candidate.append(remaining.data(), remaining.size());
      lines.push_back(truncatedText(fontId, candidate.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    const size_t spacePos = remaining.find(' ');
    std::string_view word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining = {};
    } else {
      word = remaining.substr(0, spacePos);
      remaining.remove_prefix(spacePos + 1);
    }

    candidate = currentLine;
    if (!candidate.empty()) candidate.push_back(' ');
    candidate.append(word.data(), word.size());

    if (getTextWidth(fontId, candidate.c_str(), style) <= maxWidth) {
      currentLine = candidate;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        candidate.assign(word.data(), word.size());
        if (getTextWidth(fontId, candidate.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, candidate.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine.assign(word.data(), word.size());
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        candidate.assign(word.data(), word.size());
        lines.push_back(truncatedText(fontId, candidate.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

void GfxRenderer::tapToLogical(float nx, float ny, int& outX, int& outY) const {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't.
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(int fontId, const EpdFontFamily::Style style) const {
  // Metrics must follow a rebound id, or a row is measured in the built-in face and
  // then drawn in the SD one. See setPreferredFont().
  fontId = resolveFontFamilyId(fontId);
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    return fp4::toPixel(getSdCardSpaceAdvance(*sdIt->second, style));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getSpaceAdvance(int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Metrics must follow a rebound id (see setPreferredFont()).
  fontId = resolveFontFamilyId(fontId);
  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    return fp4::toPixel(getSdCardSpaceAdvance(*sdIt->second, style));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(int fontId, const uint32_t leftCp, const uint32_t rightCp, const EpdFontFamily::Style style,
                            const int8_t tracking) const {
  fontId = resolveFontFamilyId(fontId);
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);      // 4.4 fixed-point
  return fp4::toPixel(kernFP) + trackingBetween(leftCp, rightCp, tracking);  // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, const int8_t tracking,
                                 const BidiUtils::BidiBaseDir baseDir, const TextMeasureMode mode) const {
  // Match the font drawText would use for CJK-bearing strings (see resolveTextFontId).
  // Measure the exact codepoint stream drawText renders: bidi-reordered and
  // Arabic-shaped (contextual presentation forms, Lam-Alef collapse).
  // Measuring the raw logical text counts the Alef a ligature absorbs and
  // uses base-letter advances instead of presentation-form advances, so RTL
  // lines come out wider than they draw — uneven word gaps and a ragged
  // right margin.
  std::string visual;
  text = resolveVisualText(text, visual, baseDir);
  const int resolvedFontId = resolveTextFontId(fontId, text, style);

  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(resolvedFontId);
  if (mode == TextMeasureMode::Layout && sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    int widthPx = 0;
    int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: previous glyph's advance
    bool havePrev = false;
    uint32_t prevCp = 0;
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
      if (missingGlyph::isCombining(cp)) {
        continue;
      }
      // Differential rounding: snap each glyph step to a pixel individually, matching
      // drawText so layout (wordXpos) and render geometry agree exactly. Snapping the
      // whole sum once (as before) under-counted the rendered width whenever per-glyph
      // advances carried a >=0.5px fraction, so trailing glyphs of a word spilled into
      // the next word — visible as overlap / right-edge overflow. SD layout stays
      // kern-free by design (full kern matrix is non-resident; see
      // SdCardFont::applyKernLigaturePointers), which only ever tightens render vs
      // this measurement, never widens it.
      if (havePrev) widthPx += fp4::toPixel(prevAdvanceFP) + trackingBetween(prevCp, cp, tracking);
      // getAdvanceOrLoad (not getAdvance): resolves codepoints missing from the
      // resident advance cache by reading the glyph's advance from SD on demand,
      // instead of treating them as 0-width. A 0 here used to silently corrupt
      // layout — e.g. the CJK reference probe "我" (used to size every Han column)
      // is rarely present in the page text, so it missed the cache, measured 0,
      // and fell back to a bogus 2×"M" column width that spread ideographs far
      // apart. See SdCardFont::getAdvanceOrLoad.
      prevAdvanceFP = static_cast<int32_t>(sdIt->second->getAdvanceOrLoad(cp, styleIdx));
      // SUP/SUB glyphs render at 50% scale (renderCharScaled), so halve the advance
      // to keep measurement consistent with drawText and the builtin-font path below.
      if (isSupSub) prevAdvanceFP = (prevAdvanceFP + 1) / 2;
      havePrev = true;
      prevCp = cp;
    }
    if (havePrev) widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
    return widthPx;
  }

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  bool prevMissing = false;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
    if (missingGlyph::isCombining(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    const bool missing = glyph == nullptr;
    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = missing || prevMissing ? 0 : font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP) +
                 trackingBetween(prevCp, cp, tracking);  // snap 12.4 fixed-point to nearest pixel
    }

    prevAdvanceFP = glyph ? glyph->advanceX : missingGlyph::metrics(font.getData(style)->ascender, cp).advanceX;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }
    prevCp = cp;
    prevMissing = missing;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(int fontId) const {
  // Row height / ascender must follow a rebound id, or the row is sized for the
  // built-in face and the SD face is clipped out of it. See setPreferredFont().
  fontId = resolveFontFamilyId(fontId);
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(int fontId) const {
  // Row height must follow a rebound id (see setPreferredFont()).
  fontId = resolveFontFamilyId(fontId);
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getLineHeight(int fontId, const float compression) const {
  // Row height must follow a rebound id (see setPreferredFont()).
  fontId = resolveFontFamilyId(fontId);
  return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);
}

int GfxRenderer::getTextHeight(int fontId) const {
  // Text height must follow a rebound id (see setPreferredFont()).
  fontId = resolveFontFamilyId(fontId);
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  RICKY_PAINT_SCOPE(Text);
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  // Route CJK-bearing strings to the fallback font (see resolveTextFontId).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  if (resolvedFontId != fontId) ensureSdGlyphsResident(resolvedFontId, text, style, false);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }

  const auto& font = fontIt->second;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  bool prevMissing = false;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::anchorOverRotated90CW(anchor, lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    const uint32_t sourceCp = cp;
    cp = font.applyLigatures(cp, text, style);

    bool usedReplacement = false;
    const EpdGlyph* glyph = font.getGlyph(cp, style, &usedReplacement);
    if (usedReplacement && fontCacheManager_) {
      fontCacheManager_->reportMissingChineseCodepoint(resolvedFontId, sourceCp);
    }

    const bool missing = glyph == nullptr;
    if (prevCp != 0) {
      const auto kernFP = missing || prevMissing ? 0 : font.getKerning(prevCp, cp, style);
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);
    }
    const EpdGlyph placeholder = missing ? missingGlyph::metrics(font.getData(style)->ascender, cp) : EpdGlyph{};
    if (missing) glyph = &placeholder;

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
    prevMissing = missing;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback, DisplayRefreshContext context) const {
  absoluteGrayPlanes = false;
#ifdef SIMULATOR
  (void)context;
  display.displayGrayscaleBase(fallback, fadingFix);
#else
  display.displayGrayscaleBase(fallback, fadingFix, context);
#endif
}

bool GfxRenderer::displayGrayscaleBase(HalDisplay::GrayscaleMode mode, HalDisplay::RefreshMode fallback) const {
  absoluteGrayPlanes = false;
  if (!display.displayGrayscaleBase(mode, fallback, fadingFix)) return false;
  absoluteGrayPlanes = mode != HalDisplay::GrayscaleMode::Overlay;
  return true;
}

void GfxRenderer::preconditionGrayscale() const { display.preconditionGrayscale(); }

void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  // Rotate the logical rect's opposite corners to physical panel coords; the
  // physical bbox stays axis-aligned for all four orientations.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + w - 1, y + h - 1, &bx, &by, panelWidth, panelHeight);
  int x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
  int y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x1 < x0 || y1 < y0) return;
  display.preconditionGrayscale(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                                static_cast<uint16_t>(x1 - x0 + 1), static_cast<uint16_t>(y1 - y0 + 1));
}

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer() const {
  display.displayGrayBuffer(fadingFix);
  absoluteGrayPlanes = false;
}

void GfxRenderer::setRenderMode(RenderMode mode) {
  if (mode == BW && absoluteGrayPlanes) {
    display.cleanupGrayscaleBuffers(nullptr);  // cancel an unfinished absolute pass
    absoluteGrayPlanes = false;
  }
  renderMode = mode;
}

void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  // Guard the uint16_t casts below: a negative would wrap to a huge length.
  assert(yStart >= 0 && numRows > 0 && yStart <= static_cast<int>(panelHeight) - numRows);
  display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
}

bool GfxRenderer::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }

bool GfxRenderer::combinesGrayscaleBase() const { return ::combinesGrayscaleBase(display); }

bool GfxRenderer::supportsTextOnlyCombinedBase() const { return ::supportsTextOnlyCombinedBase(display); }

bool GfxRenderer::supportsReaderTransitions() const {
#ifdef SIMULATOR
  return false;
#else
  return display.supportsReaderTransitions();
#endif
}

bool GfxRenderer::supportsContinuousImageReading() const {
#ifdef SIMULATOR
  return false;
#else
  return display.supportsContinuousImageReading();
#endif
}

bool GfxRenderer::canUseTextTransition() const {
#ifdef SIMULATOR
  return false;
#else
  return display.canUseTextTransition();
#endif
}

void GfxRenderer::cancelGrayscale() const { ::cancelGrayscale(display); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer(const bool resyncPanelBaseline) {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  if (resyncPanelBaseline) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  struct Insets {
    int top;
    int right;
    int bottom;
    int left;
  };
  const auto boardInsets = []<typename Profile>(const Profile& profile) {
    if constexpr (requires { profile.viewableInsets; }) {
      return Insets{profile.viewableInsets.top, profile.viewableInsets.right, profile.viewableInsets.bottom,
                    profile.viewableInsets.left};
    }
    return Insets{VIEWABLE_MARGIN_TOP, VIEWABLE_MARGIN_RIGHT, VIEWABLE_MARGIN_BOTTOM, VIEWABLE_MARGIN_LEFT};
  };
  const Insets insets = boardInsets(BoardConfig::ACTIVE);
  switch (orientation) {
    case Portrait:
      *outTop = insets.top;
      *outRight = insets.right;
      *outBottom = insets.bottom;
      *outLeft = insets.left;
      break;
    case LandscapeClockwise:
      *outTop = insets.left;
      *outRight = insets.top;
      *outBottom = insets.right;
      *outLeft = insets.bottom;
      break;
    case PortraitInverted:
      *outTop = insets.bottom;
      *outRight = insets.left;
      *outBottom = insets.top;
      *outLeft = insets.right;
      break;
    case LandscapeCounterClockwise:
      *outTop = insets.right;
      *outRight = insets.bottom;
      *outBottom = insets.left;
      *outLeft = insets.top;
      break;
  }
}
