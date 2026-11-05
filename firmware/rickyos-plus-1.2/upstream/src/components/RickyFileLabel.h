#pragma once

#include <cstddef>
#include <cstring>

namespace RickyFileLabel {
inline bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xc0) == 0x80; }

// In-place, UTF-8 safe middle truncation. Preserve the end of the real name,
// including its suffix; never change the filesystem entry used by actions.
// Width is supplied by the renderer, so no font or heap dependency lives here.
template <typename Width>
void middleEllipsis(char* text, int maxWidth, Width width) {
  const size_t length = std::strlen(text);
  if (!length || width(text) <= maxWidth) return;
  constexpr char ellipsis[] = "\xe2\x80\xa6";
  const int available = maxWidth - width(ellipsis);
  if (available <= 0) {
    text[0] = '\0';
    return;
  }
  size_t suffix = length;
  const char* dot = std::strrchr(text, '.');
  if (dot && dot > text && width(dot) <= available - 2) suffix = dot - text;
  // Reserve about a third for the tail, so edition numbers survive as well.
  while (suffix > 0) {
    size_t previous = suffix - 1;
    while (previous && continuation(text[previous])) --previous;
    if (width(text + previous) > available / 3) break;
    suffix = previous;
  }
  const int prefixBudget = available - width(text + suffix);
  size_t prefix = 0;
  while (prefix < suffix) {
    size_t next = prefix + 1;
    while (next < suffix && continuation(text[next])) ++next;
    const char saved = text[next];
    text[next] = '\0';
    const int measured = width(text);
    text[next] = saved;
    if (measured > prefixBudget) break;
    prefix = next;
  }
  // A replacement must fit the original storage even with a three-byte glyph.
  if (suffix < prefix + 3) return;
  std::memmove(text + prefix + 3, text + suffix, length - suffix + 1);
  std::memcpy(text + prefix, ellipsis, 3);
}
}  // namespace RickyFileLabel
