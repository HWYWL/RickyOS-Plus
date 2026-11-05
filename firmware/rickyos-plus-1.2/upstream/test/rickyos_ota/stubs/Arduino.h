#pragma once
#include <cstddef>
#include <cstdint>
class Stream {
 public:
  size_t write(const uint8_t*, size_t count) { return count; }
};
// HttpDownloader times each request for its log line.
inline unsigned long millis() { return 0; }
