#pragma once

#include <string>

class Page;

// The opening of the page the reader stopped on, for the "上次读到这里" standby card.
// The reader hands every page it shows to remember() (RAM only, a few hundred bytes);
// persist() writes the last one beside the settings when the reader closes, so the card
// still has it after a reboot. Nothing re-opens or re-lays out the book for it.
namespace reading_excerpt {
// Keep about this many characters, cut at a sentence end where one falls nearby.
inline constexpr int kMaxChars = 80;

void remember(const std::string& bookPath, const Page& page);
void persist();
// The excerpt for `bookPath` ("" when there is none, or it belongs to another book).
std::string load(const std::string& bookPath);

// Exposed for tests: trims raw page text to a quotable excerpt (sentence-aligned start,
// at most kMaxChars characters, ending at punctuation when possible).
std::string trim(const std::string& text);
}  // namespace reading_excerpt
