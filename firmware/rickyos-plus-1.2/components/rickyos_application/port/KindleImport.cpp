#include "KindleImport.h"
// KF8 skeleton/fragment reconstruction follows foliate-js/mobi.js (MIT).
// Fixed reference and attribution: kindle-source.json, LICENSE.foliate-mobi.

#include <HalStorage.h>
#include <Logging.h>
#include <Epub/htmlEntities.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace kindle {
namespace {
constexpr uint32_t kInvalid = 0xffffffff, kMaxChapter = 2 * 1024 * 1024;
constexpr size_t kBufferSize = 32 * 1024;  // fewer, larger SD transfers for comic-sized images
constexpr uint32_t kMaxSections = 4096, kMaxFragments = 16384, kMaxToc = 4096, kMaxResources = 8192;
constexpr uint16_t kCp1252[32] = {0x20ac,0xfffd,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
  0x2c6,0x2030,0x160,0x2039,0x152,0xfffd,0x17d,0xfffd,0xfffd,0x2018,0x2019,0x201c,0x201d,0x2022,
  0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0xfffd,0x17e,0x178};
// Each bounded working buffer lives in PSRAM, never on a reader task's stack.
// Desktop uses the same algorithm with a fallible host allocation.
memory::ByteBuffer buffer(size_t bytes) {
  if (!bytes || bytes > 4 * 1024 * 1024) return {};
#ifdef SIMULATOR
  return memory::makeInternalByteBufferNoThrow(bytes);
#else
  if (!memory::psramHasHeadroom(bytes, bytes, 512 * 1024)) return {};
  return memory::makePsramByteBufferUninitializedNoThrow(bytes);
#endif
}
uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void le16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void le32(uint8_t* p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
// Built at compile time into read-only data: no RAM table. A comic copies tens of MB
// through this, where the bit-at-a-time loop cost several seconds.
constexpr auto kCrcTable = [] {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320);
    table[i] = c;
  }
  return table;
}();
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size) {
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = kCrcTable[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}
bool readAt(void* context, uint32_t offset, void* output, size_t size) {
  auto& file = *static_cast<HalFile*>(context);
  return file.seek(offset) && file.read(output, size) == static_cast<int>(size);
}
bool writeRaw(void* context, const uint8_t* data, size_t size) {
  return static_cast<HalFile*>(context)->write(data, size) == size;
}
bool lowerEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
  return true;
}
bool number(std::string_view s, uint32_t& out, unsigned base = 10) {
  if (s.empty()) return false;
  out = 0;
  for (char c : s) {
    const unsigned v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'V' ? c - 'A' + 10 :
                       c >= 'a' && c <= 'v' ? c - 'a' + 10 : base;
    if (v >= base || out > (kInvalid - v) / base) return false;
    out = out * base + v;
  }
  return true;
}

bool invalidateDerived(const std::string& cache) {
  // Section.cpp extracts images as img2_<spine>_<item>. Never let a replacement
  // at the same book path inherit old pictures. Preserve progress/user covers.
  struct Sweep { HalFile directory; char name[256] = {}; };
  auto sweep = makeUniqueNoThrow<Sweep>();
  if (!sweep) { LOG_ERR("KINDLE", "OOM: cache sweep"); return false; }
  sweep->directory = Storage.open(cache.c_str());
  if (!sweep->directory) return false;
  for (unsigned i = 0; i < 32768; ++i) {
    HalFile entry = sweep->directory.openNextFile();
    if (!entry) return true;
    const size_t length = entry.getName(sweep->name, sizeof(sweep->name));
    const bool directory = entry.isDirectory();
    entry.close(); // close before deleting this derived file
    if (!length || length >= sizeof(sweep->name)) return false;
    const std::string_view name(sweep->name);
    const bool owned = directory ? name == "sections" || name == "html" :
        name == "book.bin" || name.starts_with("css_rules.cache") || name.starts_with("img2_") ||
        name.starts_with("thumb_") || (name.starts_with("cover") && name.ends_with(".bmp"));
    if (!owned) continue;
    const std::string path = cache + "/" + sweep->name;
    if (!(directory ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str()))) return false;
  }
  return false;
}

// Streaming ZIP32/stored writer. The central directory is spooled on the card,
// not accumulated in heap. All names below are generated, not book-controlled.
class Archive {
  HalFile zip_, directory_;
  std::string directoryPath_;
  memory::ByteBuffer staging_;
  size_t used_ = 0;
  uint32_t total_ = 0, entryOffset_ = 0, entryBytes_ = 0, crc_ = 0;
  uint16_t entries_ = 0;
  char name_[80] = {};
  bool ok_ = true, active_ = false;
  bool bytes(const uint8_t* p, size_t n) {
    if (!ok_ || n > 128 * 1024 * 1024 - total_) return ok_ = false;
    total_ += n;
    while (n) {
      const size_t take = std::min(n, kBufferSize - used_);
      memcpy(staging_.get() + used_, p, take); used_ += take; p += take; n -= take;
      if (used_ == kBufferSize && !flush()) return false;
    }
    return true;
  }
 public:
  bool open(const std::string& zip, const std::string& directory) {
    directoryPath_ = directory;
    staging_ = buffer(kBufferSize);
    return staging_ && Storage.openFileForWrite("KINDLE", zip, zip_) &&
           Storage.openFileForWrite("KINDLE", directory, directory_);
  }
  bool flush() {
    if (used_ && zip_.write(staging_.get(), used_) != used_) return ok_ = false;
    used_ = 0; return ok_;
  }
  bool begin(const char* name) {
    if (active_ || entries_ >= 16000 || strlen(name) >= sizeof(name_)) return false;
    strcpy(name_, name); entryOffset_ = total_; entryBytes_ = crc_ = 0;
    uint8_t header[30] = {};
    le32(header, 0x04034b50); le16(header + 4, 20); le16(header + 6, 8);
    le16(header + 26, strlen(name));
    active_ = true;
    return bytes(header, sizeof(header)) && bytes(reinterpret_cast<const uint8_t*>(name), strlen(name));
  }
  bool put(const void* data, size_t size) {
    if (!active_ || size > kInvalid - entryBytes_) return ok_ = false;
    crc_ = crc32(crc_, static_cast<const uint8_t*>(data), size); entryBytes_ += size;
    return bytes(static_cast<const uint8_t*>(data), size);
  }
  bool text(std::string_view s) { return put(s.data(), s.size()); }
  bool end() {
    if (!active_) return false;
    uint8_t descriptor[16] = {};
    le32(descriptor, 0x08074b50); le32(descriptor + 4, crc_);
    le32(descriptor + 8, entryBytes_); le32(descriptor + 12, entryBytes_);
    if (!bytes(descriptor, sizeof(descriptor))) return false;
    uint8_t central[46] = {};
    le32(central, 0x02014b50); le16(central + 4, 20); le16(central + 6, 20); le16(central + 8, 8);
    le32(central + 16, crc_); le32(central + 20, entryBytes_); le32(central + 24, entryBytes_);
    le16(central + 28, strlen(name_)); le32(central + 42, entryOffset_);
    if (directory_.write(central, sizeof(central)) != sizeof(central) ||
        directory_.write(reinterpret_cast<const uint8_t*>(name_), strlen(name_)) != strlen(name_)) return ok_ = false;
    active_ = false; ++entries_; return ok_;
  }
  bool finish() {
    if (active_ || !ok_) return false;
    directory_.flush(); directory_.close();
    if (!Storage.openFileForRead("KINDLE", directoryPath_, directory_)) return false;
    const uint32_t directoryOffset = total_, directorySize = directory_.fileSize();
    // The output buffer cannot also serve as the read buffer.
    auto transfer = buffer(kBufferSize);
    if (!transfer) return false;
    for (uint32_t off = 0; off < directorySize;) {
      const size_t n = std::min(uint32_t(kBufferSize), directorySize - off);
      if (directory_.read(transfer.get(), n) != static_cast<int>(n) || !bytes(transfer.get(), n)) return false;
      off += n;
    }
    uint8_t end[22] = {};
    le32(end, 0x06054b50); le16(end + 8, entries_); le16(end + 10, entries_);
    le32(end + 12, directorySize); le32(end + 16, directoryOffset);
    if (!bytes(end, sizeof(end)) || !flush()) return false;
    zip_.flush(); return ok_;
  }
};

struct Section { uint32_t offset, length, fragments, firstFragment; };
struct Fragment { uint32_t insert, offset, length, section, finalOffset; };
struct Toc { uint32_t offset, fragment, label, section; };
struct Position { uint32_t fragment, offset, section; };

class Importer {
 public:
  Document document;
  HalFile source, raw;
  Input input;
  Error error = Error::None;
  memory::ByteBuffer work, sectionData, sectionsData, fragmentsData, tocData, resourcesData;
  memory::ByteBuffer positionsData;
  Position* positions = nullptr;
  uint32_t positionCount = 0;
  Section* sections = nullptr;
  Fragment* fragments = nullptr;
  Toc* toc = nullptr;
  uint8_t* resources = nullptr; // 1=JPEG, 2=PNG; unsupported resources are not claimed in OPF.
  uint32_t sectionCount = 0, fragmentCount = 0, tocCount = 0, resourceCount = 0;
  Archive zip;
  // Fixed parser workspace is owned by this checked heap object, not task stack.
  char tag[32] = {}, attribute[48] = {}, converted[96] = {}, label[1024] = {};
  char stack[128][32] = {};
  unsigned depth = 0;
  bool fail(Error e = Error::Invalid) { if (error == Error::None) error = e; return false; }
  bool cancelled() { return input.cancellation.isCancelled() ? !fail(Error::Cancelled) : false; }
  bool open(const std::string& path, CancelCheck cancellation) {
    if (!Storage.openFileForRead("KINDLE", path, source) || source.fileSize64() > 128 * 1024 * 1024)
      return fail(Error::Io);
    input = {&source, static_cast<uint32_t>(source.fileSize64()), readAt, cancellation};
    if (!document.open(input)) return fail(document.error());
    work = buffer(kBufferSize);
    return work ? true : fail(Error::OutOfMemory);
  }
  bool allocate(memory::ByteBuffer& data, size_t bytes) {
    data = buffer(bytes); return data ? true : fail(Error::OutOfMemory);
  }
  // Whole-file CRC: the v2 cache marker's identity, kept to adopt old caches once.
  bool fingerprint(uint32_t& crc) {
    crc = 0;
    for (uint32_t off = 0; off < input.size;) {
      const size_t n = std::min(uint32_t(kBufferSize), input.size - off);
      if (cancelled()) return false;
      if (!readAt(&source, off, work.get(), n)) return fail(Error::Io);
      crc = crc32(crc, work.get(), n); off += n;
    }
    return true;
  }
  // Identity checked on every open: the PDB header and record table (dates, record
  // offsets), the first and last 64 KiB and 16 evenly spaced 4 KiB samples, with the
  // size beside it in the marker. Reading the whole file instead took most of the open
  // time of a 60 MB comic whose cache was already built.
  bool identity(uint32_t& crc) {
    crc = 0;
    const auto span = [&](uint32_t off, uint32_t length) {
      while (length) {
        const size_t n = std::min<uint32_t>(kBufferSize, length);
        if (cancelled()) return false;
        if (!readAt(&source, off, work.get(), n)) return fail(Error::Io);
        crc = crc32(crc, work.get(), n); off += n; length -= n;
      }
      return true;
    };
    constexpr uint32_t kEdge = 64 * 1024, kSample = 4096, kSamples = 16;
    const uint32_t table = std::min<uint32_t>(input.size, 78 + document.absoluteRecordCount() * 8);
    if (!span(0, std::max(table, std::min(input.size, kEdge)))) return false;
    if (input.size > kEdge && !span(input.size - kEdge, kEdge)) return false;
    for (uint32_t i = 1; i <= kSamples && input.size > kSample; ++i) {
      if (!span(uint32_t(uint64_t(input.size - kSample) * i / (kSamples + 1)), kSample)) return false;
    }
    return true;
  }
  static bool count(void* ctx, uint32_t, const IndexRow&) { return ++*static_cast<uint32_t*>(ctx) <= kMaxFragments; }
  static bool skeleton(void* ctx, uint32_t i, const IndexRow& row) {
    auto& s = *static_cast<Importer*>(ctx);
    if (i >= s.sectionCount || row.counts[1] < 1 || row.counts[6] < 2) {
      LOG_ERR("KINDLE", "Invalid skeleton row %u: %u/%u", unsigned(i), row.counts[1], row.counts[6]); return false;
    }
    s.sections[i] = {row.values[6][0], row.values[6][1], row.values[1][0], 0}; return true;
  }
  static bool fragment(void* ctx, uint32_t i, const IndexRow& row) {
    auto& s = *static_cast<Importer*>(ctx); uint32_t insert;
    if (i >= s.fragmentCount || row.counts[6] < 2 || !number(row.name, insert)) {
      LOG_ERR("KINDLE", "Invalid fragment row %u: %u", unsigned(i), row.counts[6]); return false;
    }
    s.fragments[i] = {insert, row.values[6][0], row.values[6][1], kInvalid, 0}; return true;
  }
  static bool navigation(void* ctx, uint32_t i, const IndexRow& row) {
    auto& s = *static_cast<Importer*>(ctx);
    if (i >= s.tocCount || row.counts[3] < 1) { LOG_ERR("KINDLE", "Invalid NCX label %u", unsigned(i)); return false; }
    if (s.document.header().version == 8) {
      if (row.counts[6] < 2) { LOG_ERR("KINDLE", "Invalid NCX position %u", unsigned(i)); return false; }
      s.toc[i] = {row.values[6][1], row.values[6][0], row.values[3][0], kInvalid};
    } else {
      if (row.counts[1] < 1) return false;
      s.toc[i] = {row.values[1][0], kInvalid, row.values[3][0], kInvalid};
    }
    return true;
  }
  bool indexes() {
    const auto& h = document.header();
    if (h.version == 8) {
      if (!document.index(h.skeletons, count, &sectionCount) || !sectionCount || sectionCount > kMaxSections ||
          !document.index(h.fragments, count, &fragmentCount) || !fragmentCount || fragmentCount > kMaxFragments)
        return fail(document.error() == Error::None ? Error::Unsupported : document.error());
      if (!allocate(sectionsData, sectionCount * sizeof(Section)) ||
          !allocate(fragmentsData, fragmentCount * sizeof(Fragment))) return false;
      sections = reinterpret_cast<Section*>(sectionsData.get()); fragments = reinterpret_cast<Fragment*>(fragmentsData.get());
      if (!document.index(h.skeletons, skeleton, this) || !document.index(h.fragments, fragment, this)) return fail();
      uint32_t cursor = 0;
      for (uint32_t i = 0; i < sectionCount; ++i) {
        auto& section = sections[i]; section.firstFragment = cursor;
        if (section.fragments > fragmentCount - cursor) return fail();
        for (uint32_t j = 0; j < section.fragments; ++j) fragments[cursor++].section = i;
      }
      if (cursor != fragmentCount) return fail();
    } else {
      // Split on source page-break tags without retaining the whole text in RAM.
      sectionCount = 1; uint32_t at = 0; size_t matched = 0;
      while (at < h.textBytes) {
        const size_t n = std::min(uint32_t(kBufferSize), h.textBytes - at);
        if (raw.read(work.get(), n) != static_cast<int>(n)) return fail(Error::Io);
        for (size_t j = 0; j < n; ++j) {
          const char c = std::tolower(work.get()[j]);
          if (c == "<mbp:pagebreak"[matched]) {
            if (++matched == 14) { if (++sectionCount > kMaxSections) return fail(Error::Unsupported); matched = 0; }
          } else matched = c == '<' ? 1 : 0;
        }
        at += n;
      }
      if (!allocate(sectionsData, sectionCount * sizeof(Section))) return false;
      sections = reinterpret_cast<Section*>(sectionsData.get());
      raw.seek(0); at = 0; matched = 0; uint32_t current = 0, start = 0;
      while (at < h.textBytes) {
        const size_t n = std::min(uint32_t(kBufferSize), h.textBytes - at);
        if (raw.read(work.get(), n) != static_cast<int>(n)) return fail(Error::Io);
        for (size_t j = 0; j < n; ++j) {
          const char c = std::tolower(work.get()[j]);
          if (c == "<mbp:pagebreak"[matched]) {
            if (++matched == 14) {
              const uint32_t boundary = at + j + 1 - 14;
              sections[current++] = {start, boundary - start, 0, 0}; start = boundary; matched = 0;
            }
          } else matched = c == '<' ? 1 : 0;
        }
        at += n;
      }
      sections[current] = {start, h.textBytes - start, 0, 0};
    }
    if (h.ncx != kInvalid) {
      if (!document.index(h.ncx, count, &tocCount) || tocCount > kMaxToc) return fail(Error::Unsupported);
      if (tocCount) {
        if (!allocate(tocData, tocCount * sizeof(Toc))) return false;
        toc = reinterpret_cast<Toc*>(tocData.get());
        if (!document.index(h.ncx, navigation, this)) return fail();
      }
    }
    for (uint32_t i = 0; i < tocCount; ++i) {
      if (h.version == 8) {
        if (toc[i].fragment >= fragmentCount || toc[i].offset > fragments[toc[i].fragment].length) {
          LOG_ERR("KINDLE", "NCX %u out of range: fragment=%u offset=%u", unsigned(i), unsigned(toc[i].fragment), unsigned(toc[i].offset)); return fail();
        }
        toc[i].section = fragments[toc[i].fragment].section;
      } else {
        for (uint32_t j = 0; j < sectionCount; ++j)
          if (toc[i].offset >= sections[j].offset && toc[i].offset <= sections[j].offset + sections[j].length)
            toc[i].section = j;
        if (toc[i].section == kInvalid) return fail();
      }
    }
    return true;
  }
  bool scanPositions(bool collect, uint32_t& count) {
    const uint32_t total = document.header().textBytes;
    constexpr size_t overlap = 128, step = kBufferSize - overlap;
    for (uint32_t off = 0; off < total;) {
      if (cancelled()) return false;
      const size_t n = std::min(uint32_t(kBufferSize), total - off);
      if (!readAt(&raw, off, work.get(), n)) return fail(Error::Io);
      const std::string_view text(reinterpret_cast<const char*>(work.get()), n);
      const size_t end = total - off > kBufferSize ? step : n;
      for (size_t i = 0; i < end; ++i) {
        auto value = text.substr(i); uint32_t fid = kInvalid, offset;
        if (document.header().version == 8 && value.starts_with("kindle:pos:fid:")) {
          value.remove_prefix(15); const size_t separator = value.find(":off:");
          if (separator == value.npos || separator > 7 || !number(value.substr(0, separator), fid, 32)) continue;
          value.remove_prefix(separator + 5); size_t length = 0;
          while (length < value.size() && std::isalnum(static_cast<unsigned char>(value[length]))) ++length;
          if (!number(value.substr(0, length), offset, 32) || fid >= fragmentCount || offset > fragments[fid].length) continue;
        } else if (document.header().version < 8 && value.starts_with("filepos")) {
          value.remove_prefix(7);
          while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
          if (value.empty() || value.front() != '=') continue;
          value.remove_prefix(1);
          while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
          if (!value.empty() && (value.front() == '\'' || value.front() == '"')) value.remove_prefix(1);
          size_t length = 0;
          while (length < value.size() && std::isdigit(static_cast<unsigned char>(value[length]))) ++length;
          if (!number(value.substr(0, length), offset) || offset >= total) continue;
        } else continue;
        if (!collect) { if (++count > 8192) return fail(Error::Unsupported); continue; }
        bool duplicate = false;
        for (uint32_t j = 0; j < positionCount; ++j)
          if (positions[j].fragment == fid && positions[j].offset == offset) { duplicate = true; break; }
        if (duplicate) continue;
        uint32_t section = kInvalid;
        if (fid != kInvalid) section = fragments[fid].section;
        else for (uint32_t j = 0; j < sectionCount; ++j)
          if (offset >= sections[j].offset && offset <= sections[j].offset + sections[j].length) section = j;
        if (section == kInvalid || positionCount >= count) return fail();
        positions[positionCount++] = {fid, offset, section};
      }
      off += end;
    }
    return true;
  }
  bool collectPositions() {
    uint32_t capacity = 0;
    if (!scanPositions(false, capacity)) return false;
    if (!capacity) return true;
    if (!allocate(positionsData, capacity * sizeof(Position))) return false;
    positions = reinterpret_cast<Position*>(positionsData.get());
    return scanPositions(true, capacity);
  }
  bool positionUri(uint32_t fragment, uint32_t offset, char* out, size_t capacity) {
    for (uint32_t i = 0; i < positionCount; ++i) {
      if (positions[i].fragment != fragment || positions[i].offset != offset) continue;
      snprintf(out, capacity, "s%u.xhtml#p%u", unsigned(positions[i].section & 0x7fffffffu), unsigned(i));
      return true;
    }
    return false;
  }
  bool codepoint(uint32_t c) {
    char out[4]; size_t n;
    if (c < 128) { out[0] = c; n = 1; }
    else if (c < 2048) { out[0] = 0xc0 | c >> 6; out[1] = 0x80 | (c & 63); n = 2; }
    else { out[0] = 0xe0 | c >> 12; out[1] = 0x80 | ((c >> 6) & 63); out[2] = 0x80 | (c & 63); n = 3; }
    return zip.put(out, n);
  }
  bool escape(std::string_view s, bool preserveEntities = false, bool utf8 = false) {
    for (size_t i = 0; i < s.size(); ++i) {
      const unsigned char c = s[i];
      if (c == '&') {
        size_t end = i + 1;
        while (end < s.size() && end - i < 16 && s[end] != ';' && s[end] != ' ' && s[end] != '<') ++end;
        const auto entity = s.substr(i, end - i + 1);
        if (preserveEntities && end < s.size() && s[end] == ';' &&
            (entity == "&amp;" || entity == "&lt;" || entity == "&gt;" || entity == "&quot;" ||
             entity == "&apos;" || (entity.size() > 3 && entity[1] == '#'))) {
          if (!zip.text(entity)) return false;
          i = end;
        } else if (preserveEntities && end < s.size() && s[end] == ';') {
          const char* decoded = lookupHtmlEntity(entity.data(), entity.size());
          if (decoded) {
            if (!escape(decoded, false, true)) return false;
            i = end;
          } else if (!zip.text("&amp;")) return false;
        } else if (!zip.text("&amp;")) return false;
      } else if (c == '<') { if (!zip.text("&lt;")) return false; }
      else if (c == '>') { if (!zip.text("&gt;")) return false; }
      else if (c == '"') { if (!zip.text("&quot;")) return false; }
      else if (!utf8 && document.header().encoding == 1252 && c >= 128) {
        if (!codepoint(c < 160 ? kCp1252[c - 128] : c)) return false;
      } else if (c >= 32 || c == '\n' || c == '\r' || c == '\t') { if (!zip.put(&c, 1)) return false; }
    }
    return true;
  }
  bool resourceName(uint32_t index, char* out, size_t capacity) {
    if (index >= resourceCount || !resources[index]) return false;
    snprintf(out, capacity, "r%u.%s", unsigned(index), resources[index] == 1 ? "jpg" : "png"); return true;
  }
  bool resourceUri(std::string_view value, char* out, size_t capacity) {
    if (value.starts_with("kindle:embed:")) {
      value.remove_prefix(13); value = value.substr(0, value.find('?')); uint32_t id;
      return number(value, id, 32) && id && resourceName(id - 1, out, capacity);
    }
    if (value.starts_with("kindle:flow:")) {
      value.remove_prefix(12); value = value.substr(0, value.find('?')); uint32_t id;
      if (!number(value, id, 32) || !id || id > 256) return false;
      snprintf(out, capacity, "f%u.css", unsigned(id)); return true;
    }
    return false;
  }
  bool linkUri(std::string_view value, char* out, size_t capacity) {
    if (!value.starts_with("kindle:pos:fid:")) return false;
    value.remove_prefix(15); const size_t end = value.find(":off:"); uint32_t fid, off;
    if (end == value.npos || !number(value.substr(0, end), fid, 32) || !number(value.substr(end + 5), off, 32)) return false;
    return positionUri(fid, off, out, capacity);
  }
  bool closeTop() {
    return depth && zip.text("</") && zip.text(stack[--depth]) && zip.text(">");
  }
  bool anchors(uint32_t section, uint32_t position, uint32_t& next) {
    // Each TOC point is inserted once before the first complete HTML token at or
    // after its byte offset; never inject into an attribute or UTF-8 sequence.
    for (uint32_t i = next; i < tocCount; ++i) {
      if (toc[i].section != section || toc[i].offset == kInvalid) continue;
      const uint32_t target = document.header().version == 8 ? fragments[toc[i].fragment].finalOffset + toc[i].offset :
                              toc[i].offset - sections[section].offset;
      if (target > position) continue;
      snprintf(converted, sizeof(converted), "<span id=\"n%u\"></span>", unsigned(i));
      if (!zip.text(converted)) return false;
      // Use a separate flag: the original offset is also needed for NCX/link output.
      toc[i].section |= 0x80000000u;
    }
    for (uint32_t i = 0; i < positionCount; ++i) {
      auto& entry = positions[i];
      if (entry.section != section) continue;
      const uint32_t target = entry.fragment != kInvalid ? fragments[entry.fragment].finalOffset + entry.offset :
                              entry.offset - sections[section].offset;
      if (target > position) continue;
      snprintf(converted, sizeof(converted), "<span id=\"p%u\"></span>", unsigned(i));
      if (!zip.text(converted)) return false;
      entry.section |= 0x80000000u;
    }
    return true;
  }
  bool html(uint32_t section, const uint8_t* data, size_t size) {
    depth = 0; unsigned skip = 0; uint32_t next = 0;
    if (!zip.text("<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" ) ||
        !escape(document.header().title) || !zip.text("</title>")) return false;
    if (document.header().version == 8 && document.header().fdst != kInvalid) {
      memory::ByteBuffer fdst; size_t n;
      if (!document.record(document.header().fdst, fdst, n) || n < 12) return fail();
      const uint32_t flows = be32(fdst.get() + 8);
      if (flows > 256 || n < 12 + size_t(flows) * 8) return fail();
      for (uint32_t i = 1; i < flows; ++i) {
        snprintf(converted, sizeof(converted), "<link rel=\"stylesheet\" type=\"text/css\" href=\"f%u.css\"/>", unsigned(i));
        if (!zip.text(converted)) return false;
      }
    }
    if (!zip.text("</head><body>")) return false;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    for (size_t pos = 0; pos < size;) {
      if (cancelled()) return false;
      if (!skip && !anchors(section, pos, next)) return false;
      if (text[pos] != '<') {
        size_t end = text.find('<', pos); if (end == text.npos) end = size;
        if (!skip && !escape(text.substr(pos, end - pos), true)) return false;
        pos = end; continue;
      }
      if (text.substr(pos, 4) == "<!--") {
        const size_t end = text.find("-->", pos + 4); if (end == text.npos) return fail(); pos = end + 3; continue;
      }
      size_t end = pos + 1; char quote = 0;
      for (; end < size; ++end) {
        const char c = text[end];
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '>') break;
      }
      if (end == size) return fail();
      size_t cursor = pos + 1; bool closing = text[cursor] == '/'; if (closing) ++cursor;
      const size_t nameStart = cursor;
      while (cursor < end && !std::isspace(static_cast<unsigned char>(text[cursor])) && text[cursor] != '/') ++cursor;
      const size_t len = cursor - nameStart;
      if (!len || len >= sizeof(tag)) { pos = end + 1; continue; }
      for (size_t i = 0; i < len; ++i) tag[i] = std::tolower(static_cast<unsigned char>(text[nameStart + i]));
      tag[len] = 0;
      if (!strcmp(tag, "head") || !strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "guide")) {
        if (closing) { if (skip) --skip; } else if (text[end - 1] != '/') ++skip;
        pos = end + 1; continue;
      }
      if (skip) { pos = end + 1; continue; }
      if (!strcmp(tag, "html") || !strcmp(tag, "body") || !strcmp(tag, "mbp:pagebreak") || tag[0] == '?' || tag[0] == '!') {
        pos = end + 1; continue;
      }
      static constexpr std::string_view allowed = "|p|div|span|h1|h2|h3|h4|h5|h6|b|i|u|strong|em|s|sub|sup|small|blockquote|pre|code|a|img|br|hr|ul|ol|li|table|thead|tbody|tr|td|th|caption|font|center|";
      snprintf(converted, sizeof(converted), "|%s|", tag);
      if (allowed.find(converted) == allowed.npos) { pos = end + 1; continue; }
      if (closing) {
        unsigned found = depth;
        while (found && strcmp(stack[found - 1], tag)) --found;
        if (found) while (depth >= found) if (!closeTop()) return false;
        pos = end + 1; continue;
      }
      const bool block = !strcmp(tag, "p") || tag[0] == 'h' || !strcmp(tag, "div") || !strcmp(tag, "table") || !strcmp(tag, "ul") || !strcmp(tag, "ol");
      if (depth && ((!strcmp(stack[depth - 1], "p") && block) ||
                    (!strcmp(tag, "li") && !strcmp(stack[depth - 1], "li")) ||
                    (!strcmp(tag, "tr") && !strcmp(stack[depth - 1], "tr")) ||
                    ((!strcmp(tag, "td") || !strcmp(tag, "th")) &&
                     (!strcmp(stack[depth - 1], "td") || !strcmp(stack[depth - 1], "th"))))) if (!closeTop()) return false;
      const bool empty = !strcmp(tag, "img") || !strcmp(tag, "br") || !strcmp(tag, "hr") || text[end - 1] == '/';
      if (!zip.text("<") || !zip.text(tag)) return false;
      while (cursor < end) {
        while (cursor < end && (std::isspace(static_cast<unsigned char>(text[cursor])) || text[cursor] == '/')) ++cursor;
        const size_t a = cursor;
        while (cursor < end && !std::isspace(static_cast<unsigned char>(text[cursor])) && text[cursor] != '=' && text[cursor] != '/') ++cursor;
        const size_t alen = cursor - a;
        if (!alen) break;
        while (cursor < end && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor == end || text[cursor] != '=') continue;
        ++cursor; while (cursor < end && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        const char q = cursor < end && (text[cursor] == '\'' || text[cursor] == '"') ? text[cursor++] : 0;
        const size_t valueStart = cursor;
        while (cursor < end && (q ? text[cursor] != q : !std::isspace(static_cast<unsigned char>(text[cursor])) && text[cursor] != '>')) ++cursor;
        auto value = text.substr(valueStart, cursor - valueStart); if (q && cursor < end) ++cursor;
        if (alen >= sizeof(attribute)) continue;
        for (size_t i = 0; i < alen; ++i) {
          attribute[i] = std::tolower(static_cast<unsigned char>(text[a + i]));
        }
        attribute[alen] = 0;
        if (!strcmp(attribute, "recindex")) {
          uint32_t index; if (!number(value, index) || !index || !resourceName(index - 1, converted, sizeof(converted))) continue;
          strcpy(attribute, "src"); value = converted;
        } else if (!strcmp(attribute, "filepos")) {
          uint32_t offset; if (!number(value, offset)) continue;
          if (!positionUri(kInvalid, offset, converted, sizeof(converted))) continue;
          strcpy(attribute, "href"); value = converted;
        } else if (!strcmp(attribute, "src") || !strcmp(attribute, "href")) {
          if (resourceUri(value, converted, sizeof(converted)) || linkUri(value, converted, sizeof(converted))) value = converted;
          else if (value.starts_with("kindle:") || value.starts_with("javascript:")) continue;
        } else if (strcmp(attribute, "id") && strcmp(attribute, "class") && strcmp(attribute, "style") &&
                   strcmp(attribute, "align") && strcmp(attribute, "width") && strcmp(attribute, "height") &&
                   strcmp(attribute, "colspan") && strcmp(attribute, "rowspan") && strcmp(attribute, "alt") &&
                   strcmp(attribute, "title") && strcmp(attribute, "name")) continue;
        if (!zip.text(" ") || !zip.text(attribute) || !zip.text("=\"") || !escape(value, true) || !zip.text("\"")) return false;
      }
      if (!zip.text(empty ? "/>" : ">")) return false;
      if (!empty) {
        if (depth >= 128) return fail(Error::Unsupported);
        strcpy(stack[depth++], tag);
      }
      pos = end + 1;
    }
    if (!anchors(section, size, next)) return false;
    for (uint32_t i = 0; i < tocCount; ++i) toc[i].section &= 0x7fffffffu;
    for (uint32_t i = 0; i < positionCount; ++i) positions[i].section &= 0x7fffffffu;
    while (depth) if (!closeTop()) return false;
    return zip.text("</body></html>");
  }
  bool chapters() {
    for (uint32_t i = 0; i < sectionCount; ++i) {
      if (cancelled()) return false;
      auto& s = sections[i]; uint32_t total = s.length;
      for (uint32_t j = 0; j < s.fragments; ++j) {
        const auto& f = fragments[s.firstFragment + j];
        if (f.length > kMaxChapter - std::min(total, kMaxChapter)) return fail(Error::Unsupported);
        total += f.length;
      }
      if (total > kMaxChapter || s.offset > document.header().rawBytes || total > document.header().rawBytes - s.offset)
        return fail(Error::Unsupported);
      if (!total) {
        snprintf(converted, sizeof(converted), "s%u.xhtml", unsigned(i));
        if (!zip.begin(converted) || !html(i, nullptr, 0) || !zip.end()) return false;
        continue;
      }
      // One source chapter + one growing assembled chapter, each <=2 MiB.
      // Free both after each chapter; never retain whole-book text.
      if (!allocate(sectionData, total)) return false;
      if (!readAt(&raw, s.offset, sectionData.get(), total)) return fail(Error::Io);
      memory::ByteBuffer assembled;
      const uint8_t* htmlData = sectionData.get(); uint32_t length = total;
      if (s.fragments) {
        if (!allocate(assembled, total)) return false;
        memcpy(assembled.get(), sectionData.get(), s.length); length = s.length;
        for (uint32_t j = 0; j < s.fragments; ++j) {
          auto& f = fragments[s.firstFragment + j];
          if (f.insert < s.offset || f.insert - s.offset > length || f.offset > total - s.length ||
              f.length > total - s.length - f.offset) return fail();
          const uint32_t insert = f.insert - s.offset;
          memmove(assembled.get() + insert + f.length, assembled.get() + insert, length - insert);
          memcpy(assembled.get() + insert, sectionData.get() + s.length + f.offset, f.length);
          for (uint32_t k = 0; k < j; ++k) {
            auto& old = fragments[s.firstFragment + k]; if (old.finalOffset >= insert) old.finalOffset += f.length;
          }
          f.finalOffset = insert; length += f.length;
        }
        htmlData = assembled.get();
      }
      snprintf(converted, sizeof(converted), "s%u.xhtml", unsigned(i));
      if (!zip.begin(converted) || !html(i, htmlData, length) || !zip.end()) return false;
      sectionData.reset();
    }
    return true;
  }
  bool copyRange(uint32_t offset, uint32_t size, bool sourceFile) {
    for (uint32_t at = 0; at < size;) {
      if (cancelled()) return false;
      const size_t n = std::min(uint32_t(kBufferSize), size - at);
      if (!readAt(sourceFile ? &source : &raw, offset + at, work.get(), n) || !zip.put(work.get(), n)) return fail(Error::Io);
      at += n;
    }
    return true;
  }
  bool images() {
    const auto& h = document.header();
    // Resource indices are absolute even for a combined MOBI6/KF8 container.
    if (h.resources == kInvalid) return true;
    if (h.resources >= document.absoluteRecordCount()) return fail();
    resourceCount = document.absoluteRecordCount() - h.resources;
    if (resourceCount > kMaxResources) return fail(Error::Unsupported);
    if (!allocate(resourcesData, resourceCount)) return false;
    resources = resourcesData.get(); memset(resources, 0, resourceCount);
    for (uint32_t i = 0; i < resourceCount; ++i) {
      uint32_t off, size; uint8_t magic[8] = {};
      if (!document.recordBounds(h.resources + i, off, size, true)) return fail();
      if (size < 8) continue;
      if (!readAt(&source, off, magic, sizeof(magic))) return fail(Error::Io);
      if (magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff) resources[i] = 1;
      else if (!memcmp(magic, "\x89PNG\r\n\x1a\n", 8)) resources[i] = 2;
      else continue;
      resourceName(i, converted, sizeof(converted));
      if (!zip.begin(converted) || !copyRange(off, size, true) || !zip.end()) return false;
    }
    return true;
  }
  bool flows() {
    const auto& h = document.header(); if (h.version != 8 || h.fdst == kInvalid) return true;
    memory::ByteBuffer data; size_t n;
    if (!document.record(h.fdst, data, n) || n < 12 || memcmp(data.get(), "FDST", 4)) return fail();
    const uint32_t count = be32(data.get() + 8);
    if (!count || count > 256 || n < 12 + size_t(count) * 8) return fail();
    for (uint32_t i = 1; i < count; ++i) {
      const uint32_t begin = be32(data.get() + 12 + i * 8), end = be32(data.get() + 16 + i * 8);
      if (begin > end || end > h.rawBytes || end - begin > kMaxChapter) return fail();
      memory::ByteBuffer css;
      if (end > begin && (!allocate(css, end - begin) || !readAt(&raw, begin, css.get(), end - begin))) return fail(Error::Io);
      snprintf(converted, sizeof(converted), "f%u.css", unsigned(i));
      if (!zip.begin(converted)) return false;
      std::string_view text(reinterpret_cast<const char*>(css.get()), end - begin);
      for (size_t p = 0; p < text.size();) {
        const size_t start = text.find("kindle:", p);
        if (start == text.npos) { if (!zip.text(text.substr(p))) return false; break; }
        if (!zip.text(text.substr(p, start - p))) return false;
        size_t finish = start;
        while (finish < text.size() && text[finish] != '"' && text[finish] != '\'' && text[finish] != ')' && !std::isspace(static_cast<unsigned char>(text[finish]))) ++finish;
        if (resourceUri(text.substr(start, finish - start), converted, sizeof(converted))) {
          if (!zip.text(converted)) return false;
        } else if (!zip.text("about:blank")) return false;
        p = finish;
      }
      if (!zip.end()) return false;
    }
    return true;
  }
  bool package() {
    if (!zip.begin("META-INF/container.xml") || !zip.text("<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>") || !zip.end()) return false;
    if (!zip.begin("toc.ncx") || !zip.text("<?xml version=\"1.0\" encoding=\"utf-8\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head/><docTitle><text>") ||
        !escape(document.header().title) || !zip.text("</text></docTitle><navMap>")) return false;
    for (uint32_t i = 0; i < tocCount; ++i) {
      if (!document.indexString(document.header().ncx, toc[i].label, label, sizeof(label))) return fail();
      snprintf(converted, sizeof(converted), "<navPoint id=\"n%u\" playOrder=\"%u\"><navLabel><text>", unsigned(i), unsigned(i + 1));
      if (!zip.text(converted) || !escape(label, true) || !zip.text("</text></navLabel><content src=\"")) return false;
      snprintf(converted, sizeof(converted), "s%u.xhtml#n%u", unsigned(toc[i].section), unsigned(i));
      if (!zip.text(converted) || !zip.text("\"/></navPoint>")) return false;
    }
    if (!zip.text("</navMap></ncx>") || !zip.end() || !zip.begin("content.opf") ||
        !zip.text("<?xml version=\"1.0\" encoding=\"utf-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"book-id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"book-id\">ricky-kindle-cache-v1</dc:identifier><dc:title>") ||
        !escape(document.header().title) || !zip.text("</dc:title><dc:creator>") || !escape(document.header().author) ||
        !zip.text("</dc:creator><dc:language>und</dc:language>")) return false;
    const uint32_t cover = document.header().cover;
    if (cover < resourceCount && resources[cover]) {
      snprintf(converted, sizeof(converted), "<meta name=\"cover\" content=\"r%u\"/>", unsigned(cover));
      if (!zip.text(converted)) return false;
    }
    if (!zip.text("</metadata><manifest><item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>")) return false;
    for (uint32_t i = 0; i < sectionCount; ++i) {
      snprintf(converted, sizeof(converted), "<item id=\"s%u\" href=\"s%u.xhtml\" media-type=\"application/xhtml+xml\"/>", unsigned(i), unsigned(i));
      if (!zip.text(converted)) return false;
    }
    for (uint32_t i = 0; i < resourceCount; ++i) if (resources[i]) {
      snprintf(converted, sizeof(converted), "<item id=\"r%u\" href=\"r%u.%s\" media-type=\"image/%s\"/>", unsigned(i), unsigned(i),
               resources[i] == 1 ? "jpg" : "png", resources[i] == 1 ? "jpeg" : "png");
      if (!zip.text(converted)) return false;
    }
    if (document.header().version == 8) {
      memory::ByteBuffer data; size_t n;
      if (!document.record(document.header().fdst, data, n) || n < 12) return fail();
      const uint32_t count = be32(data.get() + 8);
      for (uint32_t i = 1; i < count; ++i) {
        snprintf(converted, sizeof(converted), "<item id=\"f%u\" href=\"f%u.css\" media-type=\"text/css\"/>", unsigned(i), unsigned(i));
        if (!zip.text(converted)) return false;
      }
    }
    if (!zip.text("</manifest><spine toc=\"ncx\">")) return false;
    for (uint32_t i = 0; i < sectionCount; ++i) {
      snprintf(converted, sizeof(converted), "<itemref idref=\"s%u\"/>", unsigned(i)); if (!zip.text(converted)) return false;
    }
    return zip.text("</spine></package>") && zip.end();
  }
  bool build(const std::string& rawPath, const std::string& zipPath, const std::string& centralPath) {
    {
      HalFile output;
      if (!Storage.openFileForWrite("KINDLE", rawPath, output) || !document.decodeText({&output, writeRaw}))
        return fail(document.error() == Error::None ? Error::Io : document.error());
    }
    // Decoding is finished: release CDIC dictionaries before assembling a
    // chapter. Peak memory is not dictionary + two maximum-sized chapters.
    document.releaseTextDecoder();
    if (!Storage.openFileForRead("KINDLE", rawPath, raw)) return fail(Error::Io);
    if (!indexes()) { LOG_ERR("KINDLE", "Invalid chapter/navigation indexes"); return false; }
    if (!collectPositions()) { LOG_ERR("KINDLE", "Invalid internal link positions"); return false; }
    if (!zip.open(zipPath, centralPath)) return fail(Error::Io);
    if (!zip.begin("mimetype") || !zip.text("application/epub+zip") || !zip.end() ||
        !images() || !flows()) return false;
    if (!chapters()) { LOG_ERR("KINDLE", "Chapter reconstruction failed"); return false; }
    if (!package() || !zip.finish()) { LOG_ERR("KINDLE", "Package output failed"); return false; }
    return true;
  }
};
}  // namespace

bool supported(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  return dot != path.npos && (lowerEqual(path.substr(dot), ".mobi") || lowerEqual(path.substr(dot), ".azw3"));
}
bool metadata(const std::string& path, std::string& title, std::string& author, Error& error) {
  auto importer = makeUniqueNoThrow<Importer>();
  if (!importer) { error = Error::OutOfMemory; LOG_ERR("KINDLE", "OOM: metadata reader"); return false; }
  if (!importer->open(path, {})) { error = importer->error; return false; }
  const auto decode = [&](const char* input, std::string& output) {
    if (importer->document.header().encoding == 65001) { output = input; return; }
    // Metadata fields are capped at 511 bytes; the existing 8 KiB import buffer
    // safely holds their at-most-1533-byte CP1252 -> UTF-8 expansion.
    auto* bytes = importer->work.get(); size_t n = 0;
    for (; *input; ++input) {
      const uint8_t b = static_cast<uint8_t>(*input);
      const uint32_t c = b >= 128 && b < 160 ? kCp1252[b - 128] : b;
      if (c < 128) bytes[n++] = c;
      else if (c < 2048) { bytes[n++] = 0xc0 | c >> 6; bytes[n++] = 0x80 | (c & 63); }
      else { bytes[n++] = 0xe0 | c >> 12; bytes[n++] = 0x80 | ((c >> 6) & 63); bytes[n++] = 0x80 | (c & 63); }
    }
    output.assign(reinterpret_cast<const char*>(bytes), n);
  };
  decode(importer->document.header().title, title); decode(importer->document.header().author, author);
  error = Error::None; return true;
}
bool prepare(const std::string& path, const std::string& cache, std::string& archive,
             bool& rebuilt, Error& error, CancelCheck cancellation) {
  rebuilt = false; archive.clear(); error = Error::None;
  auto importer = makeUniqueNoThrow<Importer>();
  if (!importer) { error = Error::OutOfMemory; LOG_ERR("KINDLE", "OOM: import reader"); return false; }
  if (!importer->open(path, cancellation)) { error = importer->error; return false; }
  uint32_t checksum;
  if (!importer->identity(checksum)) { error = importer->error; return false; }
  const std::string finalPath = cache + "/kindle.epub", markerPath = cache + kMarkerName;
  uint8_t expected[16] = {'R','K','I',3}, actual[16] = {};
  le32(expected + 4, importer->input.size); le32(expected + 8, checksum);
  const auto writeMarker = [&] {
    HalFile marker;
    return Storage.openFileForWrite("KINDLE", markerPath, marker) &&
           marker.write(expected, sizeof(expected)) == sizeof(expected);
  };
  {
    HalFile marker;
    if (Storage.exists(finalPath.c_str()) && Storage.openFileForRead("KINDLE", markerPath, marker) &&
        marker.fileSize() == sizeof(actual) && marker.read(actual, sizeof(actual)) == sizeof(actual)) {
      marker.close();
      if (!memcmp(expected, actual, sizeof(actual))) { archive = finalPath; return true; }
      // A cache built before the sampled identity (v2, whole-file CRC): verify it the old
      // way once and adopt it, rather than converting the whole book again.
      uint8_t legacy[16] = {'R','K','I',2};
      uint32_t whole;
      le32(legacy + 4, importer->input.size);
      if (!memcmp(legacy, actual, 8)) {
        if (!importer->fingerprint(whole)) { error = importer->error; return false; }
        le32(legacy + 8, whole);
        if (!memcmp(legacy, actual, sizeof(actual)) && writeMarker()) {
          LOG_INF("KINDLE", "Adopted reading cache from the previous marker");
          archive = finalPath; return true;
        }
      }
    }
  }
  if (!Storage.ensureDirectoryExists(cache.c_str())) { error = Error::Io; return false; }
  const std::string rawPath = cache + "/.kindle.raw.tmp", zipPath = cache + "/.kindle.epub.tmp", central = cache + "/.kindle.central.tmp";
  const ScopedCleanup cleanup{[&] {
    Storage.remove(rawPath.c_str()); Storage.remove(zipPath.c_str()); Storage.remove(central.c_str());
  }};
  const bool ok = importer->build(rawPath, zipPath, central);
  error = importer->error;
  importer.reset(); // close every output before replacing/removing its file
  if (!ok) {
    if (error == Error::None) error = Error::Io;
    LOG_ERR("KINDLE", "Import failed (%u)", static_cast<unsigned>(error)); return false;
  }
  // Marker is published last. Interrupted rebuilds cannot validate stale caches.
  if (Storage.exists(markerPath.c_str()) && !Storage.remove(markerPath.c_str())) { error = Error::Io; return false; }
  if (!Storage.replaceFile(zipPath.c_str(), finalPath.c_str())) { error = Error::Io; return false; }
  // Remove only derived layout data for this book, never progress or user files.
  if (!invalidateDerived(cache)) { error = Error::Io; return false; }
  if (!writeMarker()) { error = Error::Io; return false; }
  rebuilt = true; archive = finalPath;
  LOG_INF("KINDLE", "Imported source into private reading cache");
  return true;
}
}  // namespace kindle
