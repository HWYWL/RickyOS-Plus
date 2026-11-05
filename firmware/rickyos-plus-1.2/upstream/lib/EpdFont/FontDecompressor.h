#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  FontDecompressor() = default;
  ~FontDecompressor();
  FontDecompressor(const FontDecompressor&) = delete;
  FontDecompressor& operator=(const FontDecompressor&) = delete;
  FontDecompressor(FontDecompressor&&) = delete;
  FontDecompressor& operator=(FontDecompressor&&) = delete;

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();
  // Drop only the reader page buffer; hot groups stay for the next lookup.
  void clearPageCache() { freePageBuffer(); }

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Hot groups: recently decompressed groups (byte-aligned) for the non-prewarmed path,
  // which is every UI screen (only reader-like activities prewarm). With one slot, text
  // that mixes glyphs from a few groups re-inflated a whole group (~16 KB) per glyph
  // change; built-in UI fonts packed this way made a menu paint take over a second.
  // Least-recently-used slots are reused. Each slot owns nothrow high-water malloc
  // buffers, NOT std::vector: getBitmap() runs on the render path, and under
  // -fno-exceptions a vector resize that hits OOM abort()s the firmware instead of failing
  // (field crash: hotGroup.resize() -> std::bad_alloc -> abort with ~11 KB free).
#if FREEINK_DEVICE_METALIO_PLUS
  // The Plus packs every built-in UI face to fit its 5 MiB slot (160 groups in all), so
  // eight slots thrashed: each tab paint re-inflated its groups (~15 ms per text call).
  // One slot per group: each inflates once, <= ~2.5 MB PSRAM, ~6 KB of slot headers.
  static constexpr uint8_t HOT_GROUP_SLOTS = 160;
#elif defined(RICKYOS_PRODUCT)
  static constexpr uint8_t HOT_GROUP_SLOTS = 8;  // <= 8 x ~16 KB, PSRAM-preferred (fiFontMalloc)
#else
  static constexpr uint8_t HOT_GROUP_SLOTS = 1;  // no-PSRAM targets keep the single group
#endif
  struct HotGroup {
    const EpdFontData* font = nullptr;
    uint16_t index = UINT16_MAX;
    uint8_t* data = nullptr;
    uint32_t capacity = 0;
    // Aligned byte offset of each glyph in the group, so a fetch is a lookup instead of a
    // walk over every preceding glyph (getAlignedOffset()). Entry 0 is 0 by construction.
    uint32_t* offsets = nullptr;
    uint32_t offsetsCapacity = 0;
    // Frequency-grouped fonts only: the group's glyph indices in ascending order, parallel
    // to offsets (members are scattered over the glyph array, so position != index - first).
    uint32_t* members = nullptr;
    uint32_t membersCapacity = 0;
    bool offsetsValid = false;
    uint32_t lastUse = 0;
  };
  HotGroup hotGroups[HOT_GROUP_SLOTS] = {};
  uint32_t hotGroupClock = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as hotGroup.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

  // Grow (never shrink) an owned buffer to at least `needed` elements; false on OOM, buffer freed.
  // Templated on the element type so the glyph offset table shares the same grow-only contract.
  template <typename T>
  static bool ensureCapacity(T*& buf, uint32_t& capacity, uint32_t needed);

  void freePageBuffer();
  void freeHotGroup();
  static uint32_t totalGlyphs(const EpdFontData* fontData);
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height, bool is4Bit);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
