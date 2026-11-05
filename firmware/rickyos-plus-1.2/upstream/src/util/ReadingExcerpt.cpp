#include "ReadingExcerpt.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <HalStorage.h>

#include <cstring>
#include <vector>

namespace reading_excerpt {
namespace {
constexpr char kPath[] = "/.crosspoint/standby-excerpt.txt";
std::string g_book;
std::string g_text;
bool g_dirty = false;

// Byte offsets of each UTF-8 character start.
std::vector<size_t> characters(const std::string& text) {
  std::vector<size_t> starts;
  starts.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i)
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) starts.push_back(i);
  return starts;
}

bool sentenceEnd(const std::string& text, const size_t at, const size_t length) {
  static const char* const kEnds[] = {"。", "！", "？", "…", "」", "”", ".", "!", "?"};
  for (const char* end : kEnds)
    if (std::strlen(end) == length && text.compare(at, length, end) == 0) return true;
  return false;
}

bool clauseEnd(const std::string& text, const size_t at, const size_t length) {
  static const char* const kEnds[] = {"，", "；", "：", "、", ",", ";", ":"};
  for (const char* end : kEnds)
    if (std::strlen(end) == length && text.compare(at, length, end) == 0) return true;
  return false;
}

bool asciiWord(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
}  // namespace

std::string trim(const std::string& raw) {
  std::string text;
  text.reserve(raw.size());
  for (const char c : raw) {
    if (c == '\n' || c == '\r' || c == '\t') {
      if (!text.empty() && text.back() != ' ') text += ' ';
    } else {
      text += c;
    }
  }
  while (!text.empty() && text.front() == ' ') text.erase(0, 1);
  if (text.empty()) return {};
  auto starts = characters(text);
  const auto length = [&](const size_t i) {
    return (i + 1 < starts.size() ? starts[i + 1] : text.size()) - starts[i];
  };
  // A page usually opens mid-sentence: start after the first sentence end within the
  // first third, so the quote reads whole; otherwise mark the cut with an ellipsis.
  size_t first = 0;
  bool cut = true;
  for (size_t i = 0; i < starts.size() && i < starts.size() / 3; ++i) {
    if (sentenceEnd(text, starts[i], length(i))) {
      first = i + 1;
      while (first < starts.size() && (text[starts[first]] == ' ' || sentenceEnd(text, starts[first], length(first))))
        ++first;
      cut = false;
      break;
    }
  }
  if (first >= starts.size()) {
    first = 0;
    cut = true;
  }
  // Up to kMaxChars characters, ending at the last sentence end (or clause end) in reach.
  size_t last = std::min(starts.size(), first + kMaxChars);
  bool ends = last == starts.size();
  if (!ends) {
    size_t best = 0;
    for (size_t i = last; i > first + kMaxChars / 2; --i) {
      if (sentenceEnd(text, starts[i - 1], length(i - 1))) {
        best = i;
        break;
      }
    }
    if (!best)
      for (size_t i = last; i > first + kMaxChars / 2; --i)
        if (clauseEnd(text, starts[i - 1], length(i - 1))) {
          best = i;
          break;
        }
    if (best) {
      last = best;
      ends = sentenceEnd(text, starts[last - 1], length(last - 1));
    }
  }
  const size_t from = starts[first];
  const size_t to = last < starts.size() ? starts[last] : text.size();
  std::string out = text.substr(from, to - from);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty()) return {};
  if (cut && first == 0 && from == 0) out.insert(0, "……");
  if (!ends) {
    // Drop a dangling clause mark before the ellipsis.
    const auto tail = characters(out);
    if (!tail.empty() && clauseEnd(out, tail.back(), out.size() - tail.back())) out.erase(tail.back());
    out += "……";
  }
  return out;
}

void remember(const std::string& bookPath, const Page& page) {
  std::string raw;
  raw.reserve(512);
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* word = block->wordText(i);
      const uint16_t length = block->wordTextLen(i);
      if (!length) continue;
      // Latin words were split on spaces; CJK runs join directly.
      if (!raw.empty() && asciiWord(raw.back()) && asciiWord(word[0])) raw += ' ';
      raw.append(word, length);
    }
    if (!raw.empty() && raw.back() != ' ') raw += ' ';
    if (raw.size() > 600) break;  // well past what the excerpt keeps
  }
  std::string text = trim(raw);
  if (text.empty() || (text == g_text && bookPath == g_book)) return;
  g_book = bookPath;
  g_text = std::move(text);
  g_dirty = true;
}

void persist() {
  if (!g_dirty || g_book.empty()) return;
  const std::string body = g_book + "\n" + g_text;
  if (Storage.writeFile(kPath, body.c_str())) g_dirty = false;
}

std::string load(const std::string& bookPath) {
  if (bookPath.empty()) return {};
  if (bookPath == g_book) return g_text;
  HalFile file;
  if (!Storage.openFileForRead("EXCERPT", kPath, file)) return {};
  char buffer[1024];
  const int read = file.read(buffer, sizeof(buffer) - 1);
  if (read <= 0) return {};
  buffer[read] = '\0';
  const char* newline = std::strchr(buffer, '\n');
  if (!newline || std::string(buffer, newline - buffer) != bookPath) return {};
  return std::string(newline + 1);
}
}  // namespace reading_excerpt
