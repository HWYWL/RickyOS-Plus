#pragma once

#include <cstdint>
#include <string_view>

// The open book's own fonts, as seen by layout (ChapterHtmlSlimParser, ParsedText) and by
// TextBlock::render(). The reader installs a resolver while a book is open; with none
// installed every word keeps the reader font.
namespace bookfonts {

class Resolver {
 public:
  virtual ~Resolver() = default;
  // Face slot for a CSS font-family candidate list; 0 = the reader font.
  virtual uint8_t faceForFamily(std::string_view candidates) = 0;
  // Renderer font id for `face` at the reader font `baseFontId` (and an optional em * 256
  // size, 0 = same size); baseFontId when the face cannot be used.
  virtual int fontIdFor(uint8_t face, int baseFontId, uint16_t size) = 0;
};

void setActive(Resolver* resolver);
Resolver* active();

inline int fontIdFor(const uint8_t face, const int baseFontId, const uint16_t size = 0) {
  Resolver* resolver = active();
  return (resolver && (face != 0 || size != 0)) ? resolver->fontIdFor(face, baseFontId, size) : baseFontId;
}

}  // namespace bookfonts
