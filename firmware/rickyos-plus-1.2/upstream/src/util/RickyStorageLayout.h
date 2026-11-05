#pragma once

// RickyOS keeps user files in four fixed SD folders. The Storage page opens
// them, pickers start in them, and downloads land in them.
namespace RickyStorageLayout {
inline constexpr const char* BOOKS = "/books";
inline constexpr const char* FONTS = "/fonts";
inline constexpr const char* IMAGES = "/images";
inline constexpr const char* DOWNLOADS = "/downloads";

// Creates any missing fixed folder; existing folders and files are untouched.
void ensureFolders();

// One-time move of books from the folders other firmware used (/book and
// /Pushed Books), and from WeRead's former export folder (/WeRead), into
// /books. Each book keeps its progress, bookmarks, caches, recent entry and
// statistics. A name already present in /books is skipped, never overwritten.
// Returns the number of books moved.
int migrateLegacyBooks();
}  // namespace RickyStorageLayout
