#pragma once

#include <cstddef>
#include <cstdint>

namespace txt_encoding {

enum class Encoding : uint8_t { Unknown = 0, Utf8 = 1, Gbk = 2, Utf16Le = 3, Utf16Be = 4, Unsupported = 255 };
enum class ReadError : uint8_t { None, InvalidData, Io, OutOfMemory };

bool isUtf16(Encoding encoding);
size_t encodeUtf8(uint32_t codepoint, uint8_t* output);

// Four-byte carry only; original byte offsets remain the reader's coordinates.
class Utf16Decoder {
 public:
  enum class Status { NeedMore, Ready, Invalid };
  explicit Utf16Decoder(Encoding encoding) : bigEndian_(encoding == Encoding::Utf16Be) {}
  Status push(uint8_t byte, uint32_t& codepoint);
  bool pending() const { return length_ != 0; }

 private:
  uint8_t bytes_[4] = {};
  uint8_t length_ = 0;
  bool bigEndian_;
};

struct TranscodeResult {
  size_t rawLength;
  size_t utf8Length;
};

bool isSerializedValueValid(uint8_t value);
bool isSupported(Encoding encoding);
Encoding detect(const uint8_t* data, size_t length, bool finalChunk);

// Uses one buffer for both source and destination. The caller must provide
// enough capacity for rawLength + the UTF-8 output.
TranscodeResult transcodeGbkInPlace(uint8_t* buffer, size_t rawLength, size_t capacity, bool finalChunk);

// Maps a consumed UTF-8 prefix back to bytes in the original GBK stream.
size_t gbkSourceLength(const uint8_t* utf8, size_t utf8Length);

}  // namespace txt_encoding
