#pragma once

#include <CancelCheck.h>
#include <Memory.h>
#include <cstddef>
#include <cstdint>

// Format-only reader. Storage and output ownership stay with the caller. No
// stdio, tasks, DRM keys, or whole-book text allocation are used here.
namespace kindle {
struct Input {
  void* context = nullptr;
  uint32_t size = 0;
  bool (*read)(void*, uint32_t, void*, size_t) = nullptr;
  CancelCheck cancellation;
};
struct Output {
  void* context = nullptr;
  bool (*write)(void*, const uint8_t*, size_t) = nullptr;
};
enum class Error { None, Invalid, Encrypted, Unsupported, OutOfMemory, Io, Cancelled };
struct Header {
  uint32_t version = 0, encoding = 0, textBytes = 0, rawBytes = 0;
  uint32_t resources = 0xffffffff, cover = 0xffffffff, ncx = 0xffffffff;
  uint32_t fdst = 0xffffffff, fragments = 0xffffffff, skeletons = 0xffffffff;
  uint32_t huff = 0xffffffff, huffRecords = 0, trailing = 0;
  uint16_t compression = 0, textRecords = 0, recordSize = 0;
  char title[512] = {}, author[512] = {};
};
struct IndexRow {
  char name[256] = {};
  uint32_t values[32][4] = {};
  uint8_t counts[32] = {};
};
using IndexFn = bool (*)(void*, uint32_t, const IndexRow&);

class Document {
 public:
  bool open(Input source);
  bool decodeText(Output target);
  void releaseTextDecoder() { dictionary_.reset(); dictionaryIndex_.reset(); }
  bool record(uint32_t index, memory::ByteBuffer& data, size_t& size);
  bool recordBounds(uint32_t index, uint32_t& offset, uint32_t& size, bool absolute = false);
  bool index(uint32_t firstRecord, IndexFn visit, void* context);
  bool indexString(uint32_t firstRecord, uint32_t offset, char* text, size_t capacity);
  const Header& header() const { return header_; }
  Error error() const { return error_; }
  uint32_t recordCount() const { return records_ - start_; }
  uint32_t absoluteRecordCount() const { return records_; }
  static bool palmDoc(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t& written);

 private:
  Input source_;
  Header header_;
  Error error_ = Error::None;
  uint32_t records_ = 0, start_ = 0;
  uint32_t huff1_[256] = {}, huff2_[32][2] = {};
  memory::ByteBuffer dictionary_, dictionaryIndex_;
  size_t dictionaryBytes_ = 0;
  uint32_t dictionaryCount_ = 0;
  bool fail(Error e);
  bool cancelled();
  bool read(uint32_t offset, void* data, size_t size);
  bool allocate(memory::ByteBuffer& data, size_t size);
  bool parseHeader(uint32_t& boundary);
  bool setupHuff();
  bool unhuff(const uint8_t* data, size_t size, uint8_t* output, size_t capacity, size_t& written, unsigned depth);
};
}  // namespace kindle
