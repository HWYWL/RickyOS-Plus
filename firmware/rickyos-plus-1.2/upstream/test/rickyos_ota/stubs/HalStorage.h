#pragma once
#include <cstddef>
#include <cstdint>
class HalFile {
 public:
  size_t write(const uint8_t*, size_t count) { return count; }
  bool isOpen() const { return true; }
  void close() {}
};
struct HalStorage {
  void remove(const char*) {}
  bool openFileForWrite(const char*, const char*, HalFile&) { return true; }
  bool replaceFile(const char*, const char*) { return true; }
};
inline HalStorage Storage;
