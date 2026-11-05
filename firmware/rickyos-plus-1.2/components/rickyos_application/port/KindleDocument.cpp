// Palm/MOBI field, compression and KF8 index logic adapted from foliate-js
// mobi.js, John Factotum, MIT. See LICENSE.foliate-mobi and kindle-source.json.
// This adaptation bounds every allocation, record, dictionary expansion and
// index value. It intentionally contains no encrypted-content implementation.
#include "KindleDocument.h"

#include <algorithm>
#include <cstring>

namespace kindle {
namespace {
constexpr uint32_t kMaxBook = 128u * 1024 * 1024;
constexpr size_t kMaxRecord = 256u * 1024;
constexpr size_t kMaxDictionary = 4u * 1024 * 1024;
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }
bool varint(const uint8_t* p, size_t end, size_t& pos, uint32_t& value) {
  value = 0;
  for (unsigned i = 0; i < 4 && pos < end; ++i) {
    const auto b = p[pos++];
    value = value << 7 | (b & 127);
    if (b & 128) return true;
  }
  return false;
}
void copyText(char* out, size_t capacity, const uint8_t* p, size_t size) {
  const size_t n = std::min(size, capacity - 1);
  memcpy(out, p, n);
  out[n] = 0;
  // Do not leave a UTF-8 lead byte without its continuation bytes at the cap.
  if (n < size) {
    size_t end = n;
    while (end && (p[end] & 0xc0) == 0x80) --end;
    if (end < n) out[end] = 0;
  }
}
}  // namespace

bool Document::fail(Error e) { if (error_ == Error::None) error_ = e; return false; }
bool Document::cancelled() {
  if (!source_.cancellation.isCancelled()) return false;
  fail(Error::Cancelled);
  return true;
}
bool Document::read(uint32_t offset, void* data, size_t size) {
  if (cancelled()) return false;
  if (offset > source_.size || size > source_.size - offset) return fail(Error::Invalid);
  return source_.read(source_.context, offset, data, size) || fail(Error::Io);
}
bool Document::allocate(memory::ByteBuffer& data, size_t size) {
#if defined(SIMULATOR)
  // Native preview has no PSRAM allocator. Keep the same per-allocation caps;
  // its host heap is not a claim about device headroom or memory performance.
  if (!size || size > kMaxDictionary) return fail(Error::OutOfMemory);
  data = memory::makeInternalByteBufferNoThrow(size);
#else
  if (!size || !memory::psramHasHeadroom(size, size, 512 * 1024)) return fail(Error::OutOfMemory);
  data = memory::makePsramByteBufferUninitializedNoThrow(size);
#endif
  return data != nullptr || fail(Error::OutOfMemory);
}
bool Document::recordBounds(uint32_t index, uint32_t& offset, uint32_t& size, bool absolute) {
  if (!absolute) {
    if (index >= records_ - start_) return fail(Error::Invalid);
    index += start_;
  }
  if (index >= records_) return fail(Error::Invalid);
  uint8_t entry[12];
  const size_t n = index + 1 < records_ ? 12 : 4;
  if (!read(78 + index * 8, entry, n)) return false;
  offset = be32(entry);
  const uint32_t end = n == 12 ? be32(entry + 8) : source_.size;
  if (offset < 78 + records_ * 8 || end <= offset || end > source_.size) return fail(Error::Invalid);
  size = end - offset;
  return true;
}
bool Document::record(uint32_t index, memory::ByteBuffer& data, size_t& size) {
  uint32_t offset, bytes;
  if (!recordBounds(index, offset, bytes)) return false;
  if (bytes > kMaxRecord) return fail(Error::Unsupported);
  size = bytes;
  return allocate(data, bytes) && read(offset, data.get(), bytes);
}
bool Document::parseHeader(uint32_t& boundary) {
  memory::ByteBuffer bytes;
  size_t n;
  if (!record(0, bytes, n) || n < 40) return fail(Error::Invalid);
  const uint8_t* p = bytes.get();
  if (be16(p + 12)) return fail(Error::Encrypted);
  const uint32_t headerLength = be32(p + 20);
  if (memcmp(p + 16, "MOBI", 4) || headerLength < 116 || headerLength > n - 16) return fail(Error::Invalid);
  header_.compression = be16(p);
  header_.textBytes = be32(p + 4);
  header_.textRecords = be16(p + 8);
  header_.recordSize = be16(p + 10);
  header_.encoding = be32(p + 28);
  header_.version = be32(p + 36);
  if ((header_.encoding != 65001 && header_.encoding != 1252) || header_.version > 8 ||
      (header_.compression != 1 && header_.compression != 2 && header_.compression != 17480))
    return fail(Error::Unsupported);
  if (!header_.textBytes || header_.textBytes > kMaxBook || !header_.recordSize ||
      !header_.textRecords || header_.textRecords >= records_ - start_) return fail(Error::Invalid);
  const uint32_t title = be32(p + 84), length = be32(p + 88);
  if (title > n || length > n - title) return fail(Error::Invalid);
  copyText(header_.title, sizeof(header_.title), p + title, length);
  header_.resources = be32(p + 108);
  header_.huff = be32(p + 112);
  header_.huffRecords = be32(p + 116);
  header_.trailing = headerLength >= 228 ? be32(p + 240) : 0;
  header_.ncx = headerLength >= 232 ? be32(p + 244) : 0xffffffff;
  if (header_.version == 8) {
    if (headerLength < 248) return fail(Error::Invalid);
    header_.fdst = be32(p + 192);
    header_.fragments = be32(p + 248);
    header_.skeletons = be32(p + 252);
  }
  if (headerLength >= 116 && (be32(p + 128) & 64)) {
    const size_t exth = headerLength + 16;
    if (n - exth < 12 || memcmp(p + exth, "EXTH", 4)) return fail(Error::Invalid);
    const uint32_t bytes = be32(p + exth + 4), count = be32(p + exth + 8);
    if (bytes < 12 || bytes > n - exth || count > (bytes - 12) / 8) return fail(Error::Invalid);
    size_t at = exth + 12, end = exth + bytes;
    for (uint32_t i = 0; i < count; ++i) {
      if (at > end || end - at < 8) return fail(Error::Invalid);
      const uint32_t type = be32(p + at), len = be32(p + at + 4);
      if (len < 8 || len > end - at) return fail(Error::Invalid);
      if (type == 100 && !header_.author[0]) copyText(header_.author, sizeof(header_.author), p + at + 8, len - 8);
      if (type == 503) copyText(header_.title, sizeof(header_.title), p + at + 8, len - 8);
      if (type == 121 && len == 12) boundary = be32(p + at + 8);
      if (type == 201 && len == 12) header_.cover = be32(p + at + 8);
      at += len;
    }
  }
  return true;
}
bool Document::open(Input source) {
  source_ = source; header_ = {}; error_ = Error::None; records_ = start_ = 0;
  dictionary_.reset(); dictionaryIndex_.reset(); dictionaryCount_ = 0;
  if (!source.read || source.size < 86 || source.size > kMaxBook) return fail(Error::Invalid);
  uint8_t head[78];
  if (!read(0, head, sizeof(head))) return false;
  if (memcmp(head + 60, "BOOKMOBI", 8)) return fail(Error::Invalid);
  records_ = be16(head + 76);
  if (records_ < 2 || 78 + records_ * 8 > source.size) return fail(Error::Invalid);
  // Validate every offset before any record-specific allocation/seek.
  uint32_t previous = 78 + records_ * 8;
  for (uint32_t i = 0; i < records_; ++i) {
    uint8_t raw[4]; if (!read(78 + i * 8, raw, 4)) return false;
    const uint32_t offset = be32(raw);
    if (offset < previous || offset >= source.size) return fail(Error::Invalid);
    if (i && offset == previous) return fail(Error::Invalid);
    previous = offset;
  }
  uint32_t boundary = 0xffffffff;
  if (!parseHeader(boundary)) return false;
  if (header_.version < 8 && boundary != 0xffffffff) {
    const uint32_t resources = header_.resources, cover = header_.cover;
    if (boundary >= records_) return fail(Error::Invalid);
    start_ = boundary;
    header_ = {};
    if (!parseHeader(boundary) || header_.version != 8) return fail(Error::Invalid);
    header_.resources = resources; header_.cover = cover;
  }
  header_.rawBytes = header_.textBytes;
  if (header_.version == 8) {
    // The PalmDOC text length covers flow zero only. KF8 CSS/SVG flows follow
    // it in the same compressed records and must not be mistaken for overflow.
    memory::ByteBuffer fdst; size_t size;
    if (!record(header_.fdst, fdst, size) || size < 12 || memcmp(fdst.get(), "FDST", 4)) return fail(Error::Invalid);
    const uint32_t count = be32(fdst.get() + 8);
    if (!count || count > (size - 12) / 8) return fail(Error::Invalid);
    uint32_t end = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t begin = be32(fdst.get() + 12 + i * 8), next = be32(fdst.get() + 16 + i * 8);
      if (begin != end || next <= begin || next > kMaxBook) return fail(Error::Invalid);
      end = next;
    }
    header_.rawBytes = end;
  }
  return true;
}
bool Document::palmDoc(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t& written) {
  written = 0;
  for (size_t i = 0; i < size;) {
    const uint8_t b = input[i++];
    if (b >= 1 && b <= 8) {
      if (b > size - i || b > capacity - written) return false;
      memcpy(output + written, input + i, b); i += b; written += b;
    } else if (b >= 128 && b <= 191) {
      if (i == size) return false;
      const uint16_t pair = static_cast<uint16_t>(b << 8 | input[i++]);
      const size_t distance = (pair & 0x3fff) >> 3, length = (pair & 7) + 3;
      if (!distance || distance > written || length > capacity - written) return false;
      for (size_t j = 0; j < length; ++j) { output[written] = output[written - distance]; ++written; }
    } else if (b >= 192) {
      if (capacity - written < 2) return false;
      output[written++] = 32; output[written++] = b ^ 128;
    } else {
      if (written == capacity) return false;
      output[written++] = b;
    }
  }
  return true;
}
bool Document::setupHuff() {
  if (dictionary_) return true;
  if (header_.huffRecords < 2 || header_.huffRecords > 64) return fail(Error::Invalid);
  memory::ByteBuffer raw;
  size_t n;
  if (!record(header_.huff, raw, n) || n < 16 || memcmp(raw.get(), "HUFF", 4)) return fail(Error::Invalid);
  const uint32_t a = be32(raw.get() + 8), b = be32(raw.get() + 12);
  if (a > n || n - a < 1024 || b > n || n - b < 256) return fail(Error::Invalid);
  for (unsigned i = 0; i < 256; ++i) huff1_[i] = be32(raw.get() + a + 4 * i);
  for (unsigned i = 0; i < 32; ++i) {
    huff2_[i][0] = be32(raw.get() + b + 8 * i);
    huff2_[i][1] = be32(raw.get() + b + 8 * i + 4);
  }
  raw.reset();
  dictionaryBytes_ = 0;
  for (uint32_t i = 1; i < header_.huffRecords; ++i) {
    uint32_t offset, bytes;
    if (!recordBounds(header_.huff + i, offset, bytes)) return false;
    if (bytes > kMaxRecord || bytes > kMaxDictionary - dictionaryBytes_) return fail(Error::Unsupported);
    dictionaryBytes_ += bytes;
  }
  if (!allocate(dictionary_, dictionaryBytes_)) return false;
  size_t base = 0;
  uint32_t added = 0;
  for (uint32_t i = 1; i < header_.huffRecords; ++i) {
    uint32_t offset, bytes;
    if (!recordBounds(header_.huff + i, offset, bytes) || !read(offset, dictionary_.get() + base, bytes)) return false;
    const uint8_t* p = dictionary_.get() + base;
    if (bytes < 16 || memcmp(p, "CDIC", 4)) return fail(Error::Invalid);
    const uint32_t len = be32(p + 4), count = be32(p + 8), bits = be32(p + 12);
    if (len < 16 || len > bytes || bits > 16 || !count || count > 65536 || added > count)
      return fail(Error::Invalid);
    if (i == 1) {
      dictionaryCount_ = count;
      if (!allocate(dictionaryIndex_, size_t{count} * 8)) return false;
    } else if (count != dictionaryCount_) return fail(Error::Invalid);
    const uint32_t entries = std::min(uint32_t{1} << bits, count - added);
    if (entries > (bytes - len) / 2) return fail(Error::Invalid);
    for (uint32_t j = 0; j < entries; ++j) {
      const uint32_t off = be16(p + len + j * 2);
      if (off < entries * 2 || off > bytes - len || bytes - len - off < 2) return fail(Error::Invalid);
      const uint32_t length = be16(p + len + off);
      if ((length & 0x7fff) > bytes - len - off - 2) return fail(Error::Invalid);
      const uint32_t fields[2] = {static_cast<uint32_t>(base + len + off + 2), length};
      memcpy(dictionaryIndex_.get() + size_t{added++} * 8, fields, 8);
    }
    base += bytes;
  }
  return added == dictionaryCount_ || fail(Error::Invalid);
}
bool Document::unhuff(const uint8_t* data, size_t size, uint8_t* output, size_t capacity, size_t& written, unsigned depth) {
  if (depth > 32) return fail(Error::Invalid);
  for (size_t bit = 0; bit < size * 8;) {
    if (cancelled()) return false;
    uint32_t peek = 0;
    for (unsigned j = 0; j < 32; ++j) {
      const size_t at = bit + j;
      peek = peek << 1 | (at < size * 8 ? ((data[at / 8] >> (7 - at % 8)) & 1) : 0);
    }
    const uint32_t entry = huff1_[peek >> 24];
    unsigned length = entry & 31;
    if (!length) return fail(Error::Invalid);
    uint32_t value = entry >> 8;
    if (!(entry & 128)) {
      while (length <= 32 && (peek >> (32 - length)) < huff2_[length - 1][0]) ++length;
      if (length > 32) return fail(Error::Invalid);
      value = huff2_[length - 1][1];
    }
    if (length > size * 8 - bit) break;  // Padding bits, not a complete dictionary symbol.
    bit += length;
    const uint32_t code = peek >> (32 - length);
    if (code > value || value - code >= dictionaryCount_) return fail(Error::Invalid);
    uint32_t fields[2]; memcpy(fields, dictionaryIndex_.get() + size_t{value - code} * 8, 8);
    const size_t bytes = fields[1] & 0x7fff;
    if (fields[1] & 0x8000) {
      if (bytes > capacity - written) return fail(Error::Invalid);
      memcpy(output + written, dictionary_.get() + fields[0], bytes); written += bytes;
    } else if (!unhuff(dictionary_.get() + fields[0], bytes, output, capacity, written, depth + 1)) return false;
  }
  return true;
}
bool Document::decodeText(Output target) {
  if (!target.write || error_ != Error::None) return false;
  if (header_.compression == 17480 && !setupHuff()) return false;
  memory::ByteBuffer output;
  if (!allocate(output, header_.recordSize)) return false;
  uint32_t total = 0;
  for (uint32_t i = 0; i < header_.textRecords; ++i) {
    memory::ByteBuffer input;
    size_t size;
    if (!record(i + 1, input, size)) return false;
    // Trailing entries use reverse VLQ lengths including the length bytes.
    for (uint32_t flags = header_.trailing >> 1; flags; flags >>= 1) {
      if (!(flags & 1)) continue;
      uint32_t length = 0; bool marker = false;
      const size_t begin = size > 4 ? size - 4 : 0;
      for (size_t j = begin; j < size; ++j) {
        if (input.get()[j] & 128) { length = 0; marker = true; }
        length = length << 7 | (input.get()[j] & 127);
      }
      if (!marker || !length || length > size) return fail(Error::Invalid);
      size -= length;
    }
    if (header_.trailing & 1) {
      if (!size) return fail(Error::Invalid);
      const size_t length = (input.get()[size - 1] & 3) + 1;
      if (length > size) return fail(Error::Invalid);
      size -= length;
    }
    size_t written = 0;
    if (header_.compression == 1) {
      if (size > header_.recordSize) return fail(Error::Invalid);
      memcpy(output.get(), input.get(), size); written = size;
    } else if (header_.compression == 2) {
      if (!palmDoc(input.get(), size, output.get(), header_.recordSize, written)) return fail(Error::Invalid);
    } else if (!unhuff(input.get(), size, output.get(), header_.recordSize, written, 0)) return false;
    if (total > header_.rawBytes || written > header_.rawBytes - total) return fail(Error::Invalid);
    if (cancelled() || !target.write(target.context, output.get(), written)) return fail(Error::Io);
    total += written;
  }
  return total == header_.rawBytes || fail(Error::Invalid);
}
bool Document::index(uint32_t first, IndexFn visit, void* context) {
  memory::ByteBuffer master;
  size_t n;
  if (!record(first, master, n) || n < 56 || memcmp(master.get(), "INDX", 4)) return fail(Error::Invalid);
  const uint32_t head = be32(master.get() + 4), count = be32(master.get() + 24);
  if (head > n || n - head < 12 || memcmp(master.get() + head, "TAGX", 4)) return fail(Error::Invalid);
  const uint8_t* tagx = master.get() + head;
  const uint32_t bytes = be32(tagx + 4), controlBytes = be32(tagx + 8);
  if (bytes < 12 || bytes > n - head || (bytes - 12) % 4 || !controlBytes || controlBytes > 32 ||
      !count || first >= recordCount() || count >= recordCount() - first) return fail(Error::Invalid);
  struct Scratch : IndexRow { uint32_t valueCount[32] = {}, valueBytes[32] = {}; };
  auto row = makeUniqueNoThrow<Scratch>();
  if (!row) return fail(Error::OutOfMemory);
  uint32_t ordinal = 0;
  for (uint32_t i = 0; i < count; ++i) {
    memory::ByteBuffer recordBytes;
    size_t size;
    if (!record(first + i + 1, recordBytes, size) || size < 56 || memcmp(recordBytes.get(), "INDX", 4))
      return fail(Error::Invalid);
    const uint8_t* p = recordBytes.get();
    const uint32_t idxt = be32(p + 20), entries = be32(p + 24);
    if (idxt > size || size - idxt < 4 || memcmp(p + idxt, "IDXT", 4) || entries > (size - idxt - 4) / 2)
      return fail(Error::Invalid);
    for (uint32_t j = 0; j < entries; ++j) {
      *row = {};
      size_t at = be16(p + idxt + 4 + 2 * j);
      const size_t end = j + 1 < entries ? be16(p + idxt + 6 + 2 * j) : idxt;
      if (at >= end || end > idxt || p[at] > end - at - 1) return fail(Error::Invalid);
      const size_t nameSize = p[at++];
      copyText(row->name, sizeof(row->name), p + at, nameSize); at += nameSize;
      if (controlBytes > end - at) return fail(Error::Invalid);
      const size_t controlStart = at; at += controlBytes;
      // First parse all control bytes/byte counts, then all value streams.
      auto& valueCount = row->valueCount;
      auto& valueBytes = row->valueBytes;
      unsigned cb = 0;
      for (uint32_t t = 12; t < bytes; t += 4) {
        const uint8_t tag = tagx[t], values = tagx[t + 1], mask = tagx[t + 2], terminal = tagx[t + 3];
        if (terminal & 1) { ++cb; continue; }
        if (tag >= 32 || !mask || !values || cb >= controlBytes) return fail(Error::Unsupported);
        const uint8_t v = p[controlStart + cb] & mask;
        unsigned shift = 0, bits = 0;
        for (unsigned b = mask; !(b & 1); b >>= 1) ++shift;
        for (unsigned b = mask; b; b >>= 1) bits += b & 1;
        if (v == mask && bits > 1) {
          if (!varint(p, end, at, valueBytes[tag])) return fail(Error::Invalid);
        } else valueCount[tag] = (v == mask ? 1 : v >> shift) * values;
      }
      for (uint32_t t = 12; t < bytes; t += 4) {
        const uint8_t tag = tagx[t];
        if (tagx[t + 3] & 1) continue;
        const size_t start = at;
        if (valueBytes[tag] > end - at) return fail(Error::Invalid);
        for (uint32_t k = 0; k < valueCount[tag] || at - start < valueBytes[tag]; ++k) {
          if (row->counts[tag] == 4 || !varint(p, end, at, row->values[tag][row->counts[tag]++]))
            return fail(Error::Unsupported);
        }
        if (valueBytes[tag] && at - start != valueBytes[tag]) return fail(Error::Invalid);
      }
      if (cancelled() || !visit(context, ordinal++, *row)) return false;
    }
  }
  return true;
}
bool Document::indexString(uint32_t first, uint32_t offset, char* text, size_t capacity) {
  if (!text || !capacity) return fail(Error::Invalid);
  memory::ByteBuffer master;
  size_t n;
  if (!record(first, master, n) || n < 56) return fail(Error::Invalid);
  const uint32_t count = be32(master.get() + 24), cncxs = be32(master.get() + 52), item = offset >> 16;
  if (item >= cncxs || count >= recordCount() || first >= recordCount() - count - 1) return fail(Error::Invalid);
  memory::ByteBuffer raw;
  if (!record(first + count + 1 + item, raw, n)) return false;
  size_t at = offset & 65535; uint32_t size;
  if (!varint(raw.get(), n, at, size) || size > n - at) return fail(Error::Invalid);
  copyText(text, capacity, raw.get() + at, size);
  return true;
}
}  // namespace kindle
