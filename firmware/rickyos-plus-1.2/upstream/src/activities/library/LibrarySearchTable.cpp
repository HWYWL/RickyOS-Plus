#include "LibrarySearchTable.h"

#include <LibraryText.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "util/PinyinDictionary.h"

namespace {

// PSRAM first; the desktop simulator (no PSRAM) and small tables use the heap.
memory::ByteBuffer allocate(const size_t bytes) {
  if (bytes == 0) return {};
  auto buffer = memory::makePsramByteBufferUninitializedNoThrow(bytes);
#if defined(BOARD_HAS_PSRAM)
  if (!buffer && bytes <= 64 * 1024) buffer = memory::ByteBuffer{static_cast<uint8_t*>(std::malloc(bytes))};
#else
  if (!buffer) buffer = memory::ByteBuffer{static_cast<uint8_t*>(std::malloc(bytes))};
#endif
  return buffer;
}

// One UTF-8 code point from `text` at `at`; advances `at`. Invalid bytes read as
// themselves so a damaged name cannot stall the walk.
uint32_t nextCodepoint(std::string_view text, size_t& at) {
  const auto byte = static_cast<uint8_t>(text[at++]);
  if (byte < 0x80) return byte;
  int extra = byte >= 0xF0 ? 3 : byte >= 0xE0 ? 2 : byte >= 0xC0 ? 1 : 0;
  uint32_t cp = byte & (0x3F >> extra);
  while (extra-- > 0 && at < text.size() && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80)
    cp = (cp << 6) | (static_cast<uint8_t>(text[at++]) & 0x3F);
  return cp;
}

// Pinyin initials of every character the input method knows: code point and a
// 26-bit mask of possible first letters (a polyphone sets several bits).
struct InitialEntry {
  uint16_t codepoint;
  uint32_t mask;
};

class PinyinInitials {
 public:
  bool load() {
    if (table_) return true;
    using namespace ricky_pinyin_data;
    const size_t total = std::size(characters);
    auto buffer = allocate(total * sizeof(InitialEntry));
    if (!buffer) return false;
    auto* out = reinterpret_cast<InitialEntry*>(buffer.get());
    size_t used = 0;
    for (const auto& syllable : syllables) {
      const uint32_t letter = (syllable.key >> 25) & 31;  // first of six 5-bit letters, 'a' = 1
      if (letter == 0 || letter > 26) continue;
      for (uint16_t i = 0; i < syllable.count && syllable.offset + i < total; ++i)
        out[used++] = InitialEntry{characters[syllable.offset + i], 1u << (letter - 1)};
    }
    std::sort(out, out + used, [](const InitialEntry& a, const InitialEntry& b) { return a.codepoint < b.codepoint; });
    size_t unique = 0;
    for (size_t i = 0; i < used; ++i) {
      if (unique > 0 && out[unique - 1].codepoint == out[i].codepoint)
        out[unique - 1].mask |= out[i].mask;
      else
        out[unique++] = out[i];
    }
    table_ = std::move(buffer);
    count_ = unique;
    return true;
  }
  uint32_t maskFor(const uint32_t cp) const {
    if (cp > 0xFFFF || !table_) return 0;
    const auto* begin = reinterpret_cast<const InitialEntry*>(table_.get());
    const auto* end = begin + count_;
    const auto* it = std::lower_bound(begin, end, cp, [](const InitialEntry& e, uint32_t v) { return e.codepoint < v; });
    return it != end && it->codepoint == cp ? it->mask : 0;
  }

 private:
  memory::ByteBuffer table_;
  size_t count_ = 0;
};

PinyinInitials initials;

bool isAsciiLetter(const uint32_t cp) { return cp >= 'a' && cp <= 'z'; }

// Initials of a folded string: each Chinese character contributes its pinyin
// initial(s); each Latin word contributes its first letter; numbers and
// punctuation contribute nothing, so "金田一37歲事件簿" reads j t y s s j b.
template <typename Sink>
void forEachInitial(std::string_view folded, Sink&& sink) {
  bool wordStart = true;
  for (size_t at = 0; at < folded.size();) {
    const uint32_t cp = nextCodepoint(folded, at);
    if (isAsciiLetter(cp)) {
      if (wordStart) sink(1u << (cp - 'a'));
      wordStart = false;
      continue;
    }
    wordStart = true;
    const uint32_t mask = initials.maskFor(cp);
    if (mask) sink(mask);
  }
}

bool initialsContain(const uint32_t* masks, const uint16_t count, std::string_view letters) {
  if (letters.empty() || letters.size() > count) return false;
  for (size_t start = 0; start + letters.size() <= count; ++start) {
    size_t k = 0;
    while (k < letters.size() && (masks[start + k] & (1u << (letters[k] - 'a')))) ++k;
    if (k == letters.size()) return true;
  }
  return false;
}

bool hasNonAscii(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](char c) { return static_cast<uint8_t>(c) >= 0x80; });
}

bool allAsciiLetters(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

constexpr size_t kChunkRecords = 64;  // 8 KiB of records per read

}  // namespace

bool LibrarySearchTable::build(library::LibraryIndexFile& index) {
  release();
  const auto& head = index.header();
  const uint16_t count = index.bookCount();
  if (count == 0) return false;
  const unsigned long started = millis();
  if (!initials.load()) {
    LOG_ERR("LIBSRCH", "no memory for the pinyin initials table");
    return false;
  }

  // The name blob holds the authors; read it once, sequentially.
  auto blob = allocate(head.nameLen);
  auto chunk = allocate(kChunkRecords * sizeof(library::ClixRecord));
  if (!blob || !chunk || !index.readRange(head.nameStart, blob.get(), head.nameLen)) {
    LOG_ERR("LIBSRCH", "cannot load the name section (%u B)", static_cast<unsigned>(head.nameLen));
    return false;
  }
  const std::string_view names(reinterpret_cast<const char*>(blob.get()), head.nameLen);
  const auto authorOf = [&names](const library::ClixRecord& r) -> std::string_view {
    const size_t at = static_cast<size_t>(r.nameOff) + sizeof(uint64_t) + r.nameLen;
    if (r.nameLen == 0 || at >= names.size()) return {};
    const size_t len = static_cast<uint8_t>(names[at]);
    return at + 1 + len <= names.size() ? names.substr(at + 1, len) : std::string_view{};
  };
  const auto hashOf = [&names](const library::ClixRecord& r) -> uint64_t {
    uint64_t hash = 0;
    if (static_cast<size_t>(r.nameOff) + sizeof(hash) <= names.size()) std::memcpy(&hash, names.data() + r.nameOff, sizeof(hash));
    return hash;
  };

  // Two passes over the records: sizes, then contents. Folding an author twice
  // is cheaper than holding every folded string in a growable heap buffer.
  const auto forEachRecord = [&](auto&& visit) -> bool {
    auto* records = reinterpret_cast<library::ClixRecord*>(chunk.get());
    for (uint32_t first = 0; first < count; first += kChunkRecords) {
      const uint32_t n = std::min<uint32_t>(kChunkRecords, count - first);
      if (!index.readRange(library::recordOffset(head, static_cast<uint16_t>(first)), records,
                           n * sizeof(library::ClixRecord)))
        return false;
      for (uint32_t i = 0; i < n; ++i) visit(static_cast<uint16_t>(first + i), records[i]);
    }
    return true;
  };

  size_t textBytes = 0, maskCount = 0;
  if (!forEachRecord([&](uint16_t, const library::ClixRecord& r) {
        const std::string_view title(r.fold, std::min<size_t>(r.foldLen, library::CLIX_FOLD_BYTES));
        const std::string author = library::fold(authorOf(r));
        textBytes += title.size() + author.size() + 2;
        forEachInitial(title, [&](uint32_t) { ++maskCount; });
        forEachInitial(author, [&](uint32_t) { ++maskCount; });
      })) {
    LOG_ERR("LIBSRCH", "record read failed");
    return false;
  }

  entries_ = allocate(static_cast<size_t>(count) * sizeof(Entry));
  text_ = allocate(textBytes);
  masks_ = allocate(std::max<size_t>(maskCount, 1) * sizeof(uint32_t));
  authorOrder_ = allocate(static_cast<size_t>(count) * sizeof(uint16_t));
  arrivalOrder_ = allocate(static_cast<size_t>(count) * sizeof(uint16_t));
  if (!entries_ || !text_ || !masks_ || !authorOrder_ || !arrivalOrder_ ||
      !index.readRange(library::authorOrderOffset(head, 0), authorOrder_.get(), count * sizeof(uint16_t)) ||
      !index.readRange(library::arrivalOrderOffset(head, 0), arrivalOrder_.get(), count * sizeof(uint16_t))) {
    LOG_ERR("LIBSRCH", "no memory for %u books (%u B text, %u initials)", static_cast<unsigned>(count),
            static_cast<unsigned>(textBytes), static_cast<unsigned>(maskCount));
    release();
    return false;
  }

  auto* entries = reinterpret_cast<Entry*>(entries_.get());
  auto* text = reinterpret_cast<char*>(text_.get());
  auto* masks = reinterpret_cast<uint32_t*>(masks_.get());
  size_t textAt = 0, maskAt = 0;
  bool overflow = false;
  const bool read = forEachRecord([&](uint16_t ordinal, const library::ClixRecord& r) {
    const std::string_view title(r.fold, std::min<size_t>(r.foldLen, library::CLIX_FOLD_BYTES));
    const std::string author = library::fold(authorOf(r));
    Entry& e = entries[ordinal];
    e = Entry{};
    e.pathHash = hashOf(r);
    if (textAt + title.size() + author.size() + 2 > textBytes) {
      overflow = true;
      return;
    }
    e.text = static_cast<uint32_t>(textAt);
    std::memcpy(text + textAt, title.data(), title.size());
    textAt += title.size();
    text[textAt++] = '\0';
    std::memcpy(text + textAt, author.data(), author.size());
    textAt += author.size();
    text[textAt++] = '\0';
    e.initials = static_cast<uint32_t>(maskAt);
    const auto push = [&](uint32_t mask) {
      if (maskAt < maskCount) masks[maskAt++] = mask;
      else overflow = true;
    };
    const size_t titleStart = maskAt;
    forEachInitial(title, push);
    e.titleInitials = static_cast<uint16_t>(std::min<size_t>(maskAt - titleStart, 0xFFFF));
    const size_t authorStart = maskAt;
    forEachInitial(author, push);
    e.authorInitials = static_cast<uint16_t>(std::min<size_t>(maskAt - authorStart, 0xFFFF));
  });
  if (!read || overflow) {
    LOG_ERR("LIBSRCH", "search table build failed (read=%d overflow=%d)", read ? 1 : 0, overflow ? 1 : 0);
    release();
    return false;
  }
  count_ = count;
  LOG_INF("LIBSRCH", "search table: %u books, %u B text, %u initials, %lu ms", static_cast<unsigned>(count),
          static_cast<unsigned>(textBytes), static_cast<unsigned>(maskCount), millis() - started);
  return true;
}

void LibrarySearchTable::release() {
  count_ = 0;
  entries_.reset();
  text_.reset();
  masks_.reset();
  authorOrder_.reset();
  arrivalOrder_.reset();
}

uint16_t LibrarySearchTable::ordinalForRow(const library::SortOrder order, const uint16_t row) const {
  using library::SortOrder;
  if (row >= count_) return 0xFFFF;
  const uint16_t reversed = static_cast<uint16_t>(count_ - 1 - row);
  const auto* author = reinterpret_cast<const uint16_t*>(authorOrder_.get());
  const auto* arrival = reinterpret_cast<const uint16_t*>(arrivalOrder_.get());
  uint16_t ordinal = 0xFFFF;
  switch (order) {
    case SortOrder::TitleAsc: return row;
    case SortOrder::TitleDesc: return reversed;
    case SortOrder::AuthorAsc: ordinal = author[row]; break;
    case SortOrder::AuthorDesc: ordinal = author[reversed]; break;
    case SortOrder::RecentAsc: ordinal = arrival[row]; break;
    case SortOrder::RecentDesc: ordinal = arrival[reversed]; break;
  }
  return ordinal < count_ ? ordinal : 0xFFFF;
}

uint64_t LibrarySearchTable::pathHash(const uint16_t ordinal) const {
  return ordinal < count_ ? reinterpret_cast<const Entry*>(entries_.get())[ordinal].pathHash : 0;
}

LibrarySearchTable::Query LibrarySearchTable::prepare(const std::string& raw) { return Query{library::fold(raw)}; }

bool LibrarySearchTable::matchesWord(const Entry& e, std::string_view word) const {
  const char* text = reinterpret_cast<const char*>(text_.get()) + e.text;
  const std::string_view title(text);
  const std::string_view author(text + title.size() + 1);
  if (hasNonAscii(word)) {
    // Chinese has no spaces between words: any substring of the title or author.
    return title.find(word) != std::string_view::npos || author.find(word) != std::string_view::npos;
  }
  // Latin words keep the prefix-of-a-word rule, so "dar mat" still finds a book.
  if (library::matchesQuery(title, word) || library::matchesQuery(author, word)) return true;
  if (!allAsciiLetters(word)) return false;
  const auto* masks = reinterpret_cast<const uint32_t*>(masks_.get()) + e.initials;
  return initialsContain(masks, e.titleInitials, word) ||
         initialsContain(masks + e.titleInitials, e.authorInitials, word);
}

bool LibrarySearchTable::matches(const uint16_t ordinal, const Query& query) const {
  if (ordinal >= count_) return false;
  if (query.folded.empty()) return true;
  const Entry& e = reinterpret_cast<const Entry*>(entries_.get())[ordinal];
  std::string_view rest(query.folded);
  while (!rest.empty()) {
    const size_t space = rest.find(' ');
    const std::string_view word = rest.substr(0, space);
    if (!word.empty() && !matchesWord(e, word)) return false;
    if (space == std::string_view::npos) break;
    rest.remove_prefix(space + 1);
  }
  return true;
}
