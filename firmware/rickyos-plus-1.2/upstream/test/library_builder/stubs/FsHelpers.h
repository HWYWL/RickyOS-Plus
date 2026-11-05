#pragma once

#include <cctype>
#include <cstring>
#include <string>

namespace FsHelpers {
// Case-insensitive, like the firmware implementation (BOOK.TXT is a book).
inline bool checkFileExtension(const std::string& path, const char* extension) {
  const size_t length = strlen(extension);
  if (path.size() < length) return false;
  for (size_t i = 0; i < length; ++i) {
    if (tolower(static_cast<unsigned char>(path[path.size() - length + i])) !=
        tolower(static_cast<unsigned char>(extension[i])))
      return false;
  }
  return true;
}
inline bool hasEpubExtension(const std::string& path) { return checkFileExtension(path, ".epub"); }
inline bool isProtectedPathComponent(const std::string& component) {
  return component.empty() || component.front() == '.' || component == "System Volume Information" ||
         component == "XTCache";
}
}  // namespace FsHelpers
