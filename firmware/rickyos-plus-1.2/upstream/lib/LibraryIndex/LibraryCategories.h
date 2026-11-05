#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace library {
// Separate from CLIX: refreshing or changing the sort cannot reassign categories.
// Membership uses the same complete-path fingerprint as CLIX reconciliation.
class LibraryCategories {
 public:
  static constexpr int MAX_CATEGORIES = 16;
  static constexpr size_t MAX_MEMBERS = 16384;  // CLIX_MAX_RECORDS
  static constexpr size_t NAME_BYTES = 49;  // up to 16 Chinese characters
  enum class Load { Missing, Ready, Error };
  Load load();
  const char* name(int slot) const { return slot >= 0 && slot < MAX_CATEGORIES ? names[slot] : ""; }
  uint16_t activeMask() const;
  int create(const char* name);
  bool rename(int slot, const char* name);
  bool erase(int slot);
  // Sorted, unique path fingerprints. Touch only these bits, preserving other
  // categories (including mixed selections during batch editing).
  bool assign(const uint64_t* hashes, size_t count, uint16_t touched, uint16_t enabled);
  bool selectionMasks(const uint64_t* hashes, size_t count, uint16_t& any, uint16_t& all) const;
  // One bounded fallible allocation only when a category filter is active.
  // slot -1 means all categorized books, used to implement "Uncategorized".
  bool loadHashes(int slot, std::unique_ptr<uint64_t[]>& hashes, size_t& count) const;

 private:
  char names[MAX_CATEGORIES][NAME_BYTES]{};
  uint16_t members = 0;
  bool ready = false;
  bool validName(const char* value, int except = -1) const;
  bool rewrite(const uint64_t* hashes = nullptr, size_t count = 0, uint16_t touched = 0, uint16_t enabled = 0);
};
}  // namespace library
