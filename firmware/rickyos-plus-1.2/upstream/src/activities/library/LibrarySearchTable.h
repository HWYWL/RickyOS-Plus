#pragma once

#include <LibraryIndexFile.h>
#include <Memory.h>

#include <cstdint>
#include <string>
#include <string_view>

// In-memory search over the whole library, built once when the reader first
// searches and released with the shelf.
//
// Searching the index file directly costs one or two random SD reads per book
// for every keystroke (title record, then the author in the name blob); with a
// few thousand books that is seconds per key. This table streams the record and
// name sections once (sequential reads) into PSRAM and answers every later key
// from memory: folded title, folded author, the pinyin initials of both, the
// path fingerprint for the category filter, and both on-card permutations so a
// row in any sort order maps to its record without a seek.
//
// Every allocation is fallible. A failed build leaves the table empty and the
// shelf falls back to the file-backed filter.
class LibrarySearchTable {
 public:
  bool build(library::LibraryIndexFile& index);
  void release();
  bool ready() const { return count_ != 0; }
  uint16_t size() const { return count_; }

  // Record ordinal shown at `row` in `order` (the in-memory twin of
  // LibraryIndexFile::ordinalForRow); 0xFFFF when out of range.
  uint16_t ordinalForRow(library::SortOrder order, uint16_t row) const;
  uint64_t pathHash(uint16_t ordinal) const;

  // A query prepared once per search: folded words, and for words of plain
  // ASCII letters their initials form.
  struct Query {
    std::string folded;
    bool empty() const { return folded.empty(); }
  };
  static Query prepare(const std::string& raw);
  bool matches(uint16_t ordinal, const Query& query) const;

 private:
  struct Entry {
    uint32_t text;      // title fold, '\0', author fold, '\0' in text_
    uint32_t initials;  // first mask in masks_: title initials, then author initials
    uint16_t titleInitials;
    uint16_t authorInitials;
    uint64_t pathHash;
  };
  bool matchesWord(const Entry& entry, std::string_view word) const;

  uint16_t count_ = 0;
  memory::ByteBuffer entries_;
  memory::ByteBuffer text_;
  memory::ByteBuffer masks_;
  memory::ByteBuffer authorOrder_;
  memory::ByteBuffer arrivalOrder_;
};
