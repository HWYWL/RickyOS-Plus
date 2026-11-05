#pragma once

#include <Epub/BookFonts.h>
#include <VectorFontSupport.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Epub;
class GfxRenderer;
class TtfEpdFont;

// The fonts a typeset book ships (its @font-face rules), loaded on first use.
//
// Layout asks for a face by the CSS font-family candidate list; the first candidate that
// names one of the book's faces wins (as in a browser), otherwise the reader font stays.
// A face is read from the EPUB into PSRAM only when a word actually uses it, then built as
// a TtfEpdFont at the reader's point size with the reader font as its missing-glyph fallback
// (book fonts are usually subsets of the characters the book uses).
//
// Loading is skipped while PSRAM is short: illustrations and the page cache come first.
// Every call happens on the render task or under its lock (layout and drawing alike).
class BookFontSet final : public bookfonts::Resolver {
 public:
  explicit BookFontSet(GfxRenderer& renderer) : renderer_(renderer) {}
  ~BookFontSet() override;
  BookFontSet(const BookFontSet&) = delete;
  BookFontSet& operator=(const BookFontSet&) = delete;

  // Read the book's usable faces (a declared file that exists in the EPUB, first per family).
  void open(const Epub& epub);
  bool empty() const { return faces_.empty(); }
  // A face is in use: the reader font then carries less of the text.
  bool anyLoaded() const;
  // Fingerprint of the usable faces, for the section cache key (0 = none).
  uint32_t key() const { return key_; }

  uint8_t faceForFamily(std::string_view candidates) override;
  int fontIdFor(uint8_t face, int baseFontId, uint16_t size) override;

  // PSRAM kept free when loading a face: decoded illustrations, the chapter's text and
  // the reader glyph cache need it more than a decorative face does.
  static constexpr size_t kPsramReserve = 512 * 1024;  // one illustration decode (~400 KB) plus slack
  // Glyph cache cap per book face. A face can carry whole chapters of body text (间客 sets
  // its notes in its own Song), and a page at 18 pt needs ~300 glyphs of ~0.7 KB; a smaller
  // cache re-rasterized every page. The arena grows in 64 KB steps only as glyphs arrive
  // and the engine flushes rather than grow while PSRAM is short, so this is a ceiling.
  static constexpr size_t kGlyphCacheBytes = 384 * 1024;
  static constexpr uint16_t kMaxGlyphs = 1024;
  // Free PSRAM checked before loading a face: its bytes plus the first arena step.
  static constexpr size_t kFirstArenaStep = 64 * 1024;
  // After a face is refused for PSRAM, layout and drawing keep the reader font for this
  // long instead of asking again for every word.
  static constexpr uint32_t kRetryAfterMs = 15000;
  static constexpr size_t kMaxFaceBytes = 4 * 1024 * 1024;
  static constexpr size_t kMaxFaces = 31;

 private:
  struct Face {
    std::string family;
    std::string path;
    bool failed = false;
    uint32_t refusedAt = 0;  // millis() of the last PSRAM refusal, 0 = none
    uint8_t* bytes = nullptr;  // malloc'd by Epub::readItemContentsToBytes
    size_t length = 0;
#if CROSSPOINT_VECTOR_FONTS
    std::unique_ptr<TtfEpdFont> font;
#endif
    int fontId = 0;
    uint8_t pointSize = 0;
    // Defined in the .cpp, where TtfEpdFont is complete (unique_ptr of an incomplete type).
    Face();
    ~Face();
    Face(Face&&) noexcept;
    Face& operator=(Face&&) noexcept;
  };

  void collectFaces();
  bool load(Face& face, uint8_t pointSize, int baseFontId);
  void releaseFont(Face& face);  // the TTF face only; the file bytes stay for a rebuild
  void unload(Face& face);

  GfxRenderer& renderer_;
  const Epub* epub_ = nullptr;
  bool collected_ = false;
  uint32_t key_ = 0;
  std::vector<Face> faces_;
};
