#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @page, etc.
 *
 * @font-face is recorded (family name -> font file inside the book, or a system-font
 * category from its local() names) for the reader's book-font set; font-family values
 * are kept as candidate lists in a pool and resolved to a face at layout time.
 */
class CssParser {
 public:
  enum class ParseResult : uint8_t {
    Complete,
    Partial,
    Error,
  };

  enum class CacheStatus : uint8_t {
    Missing,
    Complete,
    Partial,
    Invalid,
  };

  enum class CacheLoadResult : uint8_t {
    Complete,
    LowMemory,
    Invalid,
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 14;  // 14: background, border, border-radius

  // System-font categories named by local() sources or by family names such as 宋体 / serif.
  // The reader maps them to its own downloadable faces when installed.
  enum class FontCategory : uint8_t { None = 0, Serif = 1, Kai = 2, Sans = 3, FangSong = 4, Round = 5 };
  static FontCategory classifyFontName(std::string_view name);

  // One @font-face rule. path is the font file inside the book, resolved against the
  // stylesheet that declared it (empty when the rule names only system fonts).
  struct FontFace {
    std::string_view family;
    std::string_view path;
    FontCategory category = FontCategory::None;
  };
  static constexpr uint8_t MAX_FONT_FACES = 32;
  static constexpr uint16_t MAX_FONT_FAMILIES = 128;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return Complete unless bounded storage stopped rule growth or the source was invalid
   */
  ParseResult loadFromStream(HalFile& source, std::string_view stylesheetPath = {});

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const;

  // An element on the path from the document root: its tag and class attribute.
  struct ElementRef {
    std::string_view tag;
    std::string_view classes;
  };
  // As above, plus descendant rules ("div.bt p", "h2.mm b"; '>' is treated as a descendant)
  // matched against the ancestors, nearest first. Descendant rules apply last.
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr,
                                      const ElementRef* ancestors, size_t ancestorCount) const;
  [[nodiscard]] bool hasDescendantRules() const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);
  // Same, keeping font-family: the candidate list joins this parser's family pool.
  [[nodiscard]] CssStyle parseInlineStyleWithFamilies(std::string_view styleValue) const;

  // font-family candidate list for a CssStyle::fontFamily index (1-based); empty if unknown.
  [[nodiscard]] std::string_view familyAt(uint16_t index) const;
  [[nodiscard]] uint8_t fontFaceCount() const { return fontFaceCount_; }
  [[nodiscard]] FontFace fontFaceAt(uint8_t index) const;

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return entryCount_ == 0; }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return entryCount_; }

  /**
   * Clear all loaded rules
   */
  void clear() {
    descendantRules_.reset();
    descendantCount_ = 0;
    descendantIndexDirty_ = true;
    familyOffsets_.reset();
    familyChars_.reset();
    familyCount_ = 0;
    familyCharsUsed_ = 0;
    faces_.reset();
    faceChars_.reset();
    fontFaceCount_ = 0;
    faceCharsUsed_ = 0;
    entries_.reset();
    selectorPool_.reset();
    stylePool_.reset();
    entryCount_ = entryCapacity_ = 0;
    selectorPoolSize_ = selectorPoolCapacity_ = 0;
    styleCount_ = styleCapacity_ = 0;
    ruleGrowthStopped_ = false;
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /** Read the cache header without hydrating its rule map. */
  CacheStatus inspectCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache(bool complete) const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return Complete when loaded, LowMemory when it should be retried, otherwise Invalid
   */
  CacheLoadResult loadFromCache();

 private:
  enum class RuleInsertResult : uint8_t {
    Inserted,
    Merged,
    Limit,
    OutOfMemory,
  };

  enum class PoolResult : uint8_t {
    Ready,
    Limit,
    OutOfMemory,
  };

  struct SelectorEntry {
    uint32_t offset;
    uint16_t styleIndex;
    uint16_t length;
  };
  static_assert(sizeof(SelectorEntry) == 8);

  // Bounded flat storage keeps every growth operation fallible and avoids the
  // throwing node allocations used by std::unordered_map.
  std::unique_ptr<SelectorEntry[]> entries_;
  std::unique_ptr<char[]> selectorPool_;
  std::unique_ptr<CssStyle[]> stylePool_;
  uint16_t entryCount_ = 0;
  uint16_t entryCapacity_ = 0;
  uint32_t selectorPoolSize_ = 0;
  uint32_t selectorPoolCapacity_ = 0;
  uint16_t styleCount_ = 0;
  uint16_t styleCapacity_ = 0;
  bool ruleGrowthStopped_ = false;

  std::string cachePath;

  // Store indices of descendant selectors (contain a space), rebuilt after the store changes.
  static constexpr uint16_t MAX_DESCENDANT_RULES = 192;
  mutable std::unique_ptr<uint16_t[]> descendantRules_;
  mutable uint16_t descendantCount_ = 0;
  mutable bool descendantIndexDirty_ = true;
  void indexDescendantRules() const;

  // font-family candidate lists, deduplicated. Mutable: inline style="" attributes add
  // their lists while a chapter is laid out (the indices only live in that session's
  // pagination, which stores resolved faces, never these indices).
  static constexpr uint32_t MAX_FAMILY_CHARS = 8 * 1024;
  mutable std::unique_ptr<uint32_t[]> familyOffsets_;  // MAX_FONT_FAMILIES + 1 ends
  mutable std::unique_ptr<char[]> familyChars_;
  mutable uint16_t familyCount_ = 0;
  mutable uint32_t familyCharsUsed_ = 0;
  uint16_t internFamily(std::string_view candidates) const;

  struct FaceRecord {
    uint16_t familyOff, familyLen, pathOff, pathLen;
    FontCategory category;
  };
  static constexpr uint32_t MAX_FACE_CHARS = 4 * 1024;
  std::unique_ptr<FaceRecord[]> faces_;
  std::unique_ptr<char[]> faceChars_;
  uint8_t fontFaceCount_ = 0;
  uint32_t faceCharsUsed_ = 0;
  bool addFontFace(std::string_view family, std::string_view path, FontCategory category);

  // Internal parsing helpers
  bool restoreCacheBackupIfNeeded() const;
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  [[nodiscard]] int compareEntryToPieces(const SelectorEntry& entry, std::string_view p0, std::string_view p1,
                                         std::string_view p2) const;
  [[nodiscard]] size_t lowerBound(std::string_view p0, std::string_view p1, std::string_view p2, bool& exact) const;
  [[nodiscard]] const CssStyle* findStyle(std::string_view p0, std::string_view p1 = {},
                                          std::string_view p2 = {}) const;
  [[nodiscard]] std::string_view selectorAt(size_t index) const;
  RuleInsertResult insertOrMerge(std::string_view selector, const CssStyle& style);
  PoolResult ensureEntryCapacity(size_t needed);
  PoolResult ensureSelectorPoolCapacity(size_t needed);
  PoolResult ensureStyleCapacity(size_t needed);
  PoolResult internStyle(const CssStyle& style, uint16_t& indexOut);
  // owner (optional) keeps font-family candidate lists; without it font-family is ignored.
  static CssStyle parseDeclarations(std::string_view declBlock, const CssParser* owner = nullptr);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style, const CssParser* owner = nullptr);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
