#pragma once

#include <cstdint>
#include <memory>

namespace library {

enum class SourceKind : uint8_t { Folder = 1, RecursiveFolder = 2, Book = 3, ExcludedBook = 4 };
struct LibrarySource {
  SourceKind kind = SourceKind::Book;
  char path[256]{};
};

// Only allocated while managing/importing sources: starts at eight rules
// (~2 KB), grows fallibly to 128. Never resident on the ordinary shelf or
// placed on a task stack. Books in a folder do not consume source slots.
class LibrarySources {
 public:
  static constexpr uint16_t MAX_SOURCES = 128;
  enum class Load { Missing, Ready, Error };
  Load load();
  bool save(bool keepBackup = false) const;
  static bool finishUpdate(bool success, bool hadManifest);
  bool add(const char* path, SourceKind kind);
  bool exclude(const char* path);
  void erase(uint16_t index);
  bool accepts(const char* path, uint16_t source) const;
  bool acceptsLegacy(const char* path) const;
  bool includes(const char* path) const;
  bool sourceIncludes(uint16_t index, const char* path) const;
  uint16_t size() const { return count; }
  const LibrarySource& operator[](uint16_t index) const { return sources[index]; }
  static bool validPath(const char* path);
  // Snapshot ONLY the old catalog, not the SD card, on first opt-in. This
  // preserves existing users' shelves without silently enrolling the root.
  static bool preserveLegacy();
  // RickyOS keeps no pre-sources shelf: the Library is exactly the enrolled sources.
  // Removes a snapshot an earlier build kept (books on the card are untouched).
  static bool retireLegacy();
  // Whether the reader has enrolled anything yet (the sources file exists).
  static bool enrolled();
  static const char* legacyIndexPath();

 private:
  std::unique_ptr<LibrarySource[]> sources;
  uint16_t count = 0;
  uint16_t capacity = 0;
  bool ensureCapacity(uint16_t required);
  bool excluded(const char* path) const;
  bool covers(uint16_t source, const char* path) const;
};

}  // namespace library
