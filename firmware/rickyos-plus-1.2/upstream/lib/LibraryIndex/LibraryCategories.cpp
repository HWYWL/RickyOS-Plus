#include "LibraryCategories.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace library {
namespace {
constexpr char kPath[] = "/.crosspoint/library-categories.v1";
constexpr char kTemp[] = "/.crosspoint/library-categories.tmp";
constexpr char kBackup[] = "/.crosspoint/library-categories.bak";
constexpr size_t kHeaderBytes = 6 + LibraryCategories::MAX_CATEGORIES * LibraryCategories::NAME_BYTES;
bool readExact(HalFile& file, void* bytes, size_t count) {
  return file.read(static_cast<uint8_t*>(bytes), count) == static_cast<int>(count);
}
uint32_t digest(uint32_t hash, const uint8_t* bytes, size_t count) {
  while (count--) hash = (hash ^ *bytes++) * 16777619u;
  return hash;
}
uint64_t decode64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
  return value;
}
bool readMember(HalFile& file, uint64_t& key, uint16_t& mask) {
  uint8_t bytes[10];
  if (!readExact(file, bytes, sizeof(bytes))) return false;
  key = decode64(bytes);
  mask = bytes[8] | (uint16_t(bytes[9]) << 8);
  return true;
}
bool sortedUnique(const uint64_t* hashes, size_t count) {
  if (!hashes || count == 0 || count > LibraryCategories::MAX_MEMBERS) return false;
  for (size_t i = 1; i < count; ++i)
    if (hashes[i] <= hashes[i - 1]) return false;
  return true;
}
}  // namespace

uint16_t LibraryCategories::activeMask() const {
  uint16_t mask = 0;
  for (int i = 0; i < MAX_CATEGORIES; ++i)
    if (names[i][0]) mask |= uint16_t(1u << i);
  return mask;
}
LibraryCategories::Load LibraryCategories::load() {
  ready = false;
  members = 0;
  memset(names, 0, sizeof(names));
  if (!Storage.exists(kPath) && Storage.exists(kBackup) && !Storage.rename(kBackup, kPath)) return Load::Error;
  if (!Storage.exists(kPath)) {
    ready = true;
    return Load::Missing;
  }
  HalFile file;
  uint8_t header[6];
  if (!Storage.openFileForRead("LIBCAT", kPath, file) || !readExact(file, header, sizeof(header)) ||
      memcmp(header, "LCT1", 4) || !readExact(file, names, sizeof(names)))
    return Load::Error;
  const uint16_t total = header[4] | (uint16_t(header[5]) << 8);
  if (total > MAX_MEMBERS || file.fileSize() != kHeaderBytes + size_t(total) * 10 + 4) return Load::Error;
  for (int i = 0; i < MAX_CATEGORIES; ++i) {
    if (!memchr(names[i], 0, NAME_BYTES)) return Load::Error;
  }
  for (int i = 0; i < MAX_CATEGORIES; ++i) {
    if (names[i][0] && !validName(names[i], i)) return Load::Error;
  }
  // Count is protected by exact file length; digest covers magic/names/members.
  uint32_t hash = digest(digest(2166136261u, header, 4), reinterpret_cast<uint8_t*>(names), sizeof(names));
  uint64_t last = 0;
  const uint16_t active = activeMask();
  for (uint16_t i = 0; i < total; ++i) {
    uint8_t bytes[10];
    if (!readExact(file, bytes, sizeof(bytes))) return Load::Error;
    const uint64_t key = decode64(bytes);
    const uint16_t mask = bytes[8] | (uint16_t(bytes[9]) << 8);
    if ((i && key <= last) || !mask || (mask & ~active)) return Load::Error;
    last = key;
    hash = digest(hash, bytes, sizeof(bytes));
  }
  uint8_t tail[4];
  if (!readExact(file, tail, sizeof(tail))) return Load::Error;
  const uint32_t stored =
      uint32_t(tail[0]) | (uint32_t(tail[1]) << 8) | (uint32_t(tail[2]) << 16) | (uint32_t(tail[3]) << 24);
  if (stored != hash) return Load::Error;
  members = total;
  ready = true;
  return Load::Ready;
}
bool LibraryCategories::validName(const char* value, int except) const {
  if (!value || !value[0] || strnlen(value, NAME_BYTES) >= NAME_BYTES) return false;
  bool visible = false;
  for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    if (*p < 32 || *p == 127) return false;
    if (*p != ' ') visible = true;
  }
  if (!visible) return false;
  // Reject malformed/truncated UTF-8 imported via a damaged manifest too.
  const auto* p = reinterpret_cast<const unsigned char*>(value);
  while (*p) {
    const unsigned char lead = *p++;
    if (lead < 0x80) continue;
    const int more = lead >= 0xC2 && lead <= 0xDF   ? 1
                     : lead >= 0xE0 && lead <= 0xEF ? 2
                     : lead >= 0xF0 && lead <= 0xF4 ? 3
                                                    : -1;
    if (more < 0) return false;
    if ((lead == 0xE0 && *p < 0xA0) || (lead == 0xED && *p >= 0xA0) || (lead == 0xF0 && *p < 0x90) ||
        (lead == 0xF4 && *p >= 0x90))
      return false;
    for (int i = 0; i < more; ++i)
      if ((*p++ & 0xC0) != 0x80) return false;
  }
  for (int i = 0; i < MAX_CATEGORIES; ++i)
    if (i != except && !strcmp(names[i], value)) return false;
  return true;
}
int LibraryCategories::create(const char* value) {
  if (!ready || !validName(value)) return -1;
  for (int i = 0; i < MAX_CATEGORIES; ++i) {
    if (names[i][0]) continue;
    strcpy(names[i], value);
    if (rewrite()) return i;
    memset(names[i], 0, NAME_BYTES);
    return -1;
  }
  return -1;
}
bool LibraryCategories::rename(int slot, const char* value) {
  if (!ready || slot < 0 || slot >= MAX_CATEGORIES || !names[slot][0] || !validName(value, slot)) return false;
  char previous[NAME_BYTES];
  memcpy(previous, names[slot], NAME_BYTES);
  memset(names[slot], 0, NAME_BYTES);
  strcpy(names[slot], value);
  if (rewrite()) return true;
  memcpy(names[slot], previous, NAME_BYTES);
  return false;
}
bool LibraryCategories::erase(int slot) {
  if (!ready || slot < 0 || slot >= MAX_CATEGORIES || !names[slot][0]) return false;
  char previous[NAME_BYTES];
  memcpy(previous, names[slot], NAME_BYTES);
  memset(names[slot], 0, NAME_BYTES);
  if (rewrite()) return true;
  memcpy(names[slot], previous, NAME_BYTES);
  return false;
}
bool LibraryCategories::assign(const uint64_t* hashes, size_t count, uint16_t touched, uint16_t enabled) {
  if (!ready || !sortedUnique(hashes, count) || (touched & ~activeMask())) return false;
  return !touched || rewrite(hashes, count, touched, enabled);
}
bool LibraryCategories::rewrite(const uint64_t* hashes, size_t count, uint16_t touched, uint16_t enabled) {
  // Stream/merge the old membership file, never an all-books vector. Names are
  // < 1 KB; each disk record is 10 LE bytes, not an ABI-dependent struct.
  HalFile input, output;
  if (members && (!Storage.openFileForRead("LIBCAT", kPath, input) || !input.seek(kHeaderBytes))) return false;
  Storage.mkdir("/.crosspoint");
  if (!Storage.openFileForWrite("LIBCAT", kTemp, output)) return false;
  uint8_t header[6] = {'L', 'C', 'T', '1', 0, 0};
  if (output.write(header, 6) != 6 ||
      output.write(reinterpret_cast<const uint8_t*>(names), sizeof(names)) != sizeof(names))
    return false;
  uint16_t written = 0, oldMask = 0;
  uint32_t hash = digest(digest(2166136261u, header, 4), reinterpret_cast<const uint8_t*>(names), sizeof(names));
  uint64_t oldKey = 0;
  size_t old = 0, next = 0;
  bool haveOld = false;
  while (old < members || haveOld || next < count) {
    if (!haveOld && old < members) {
      if (!readMember(input, oldKey, oldMask)) return false;
      ++old;
      haveOld = true;
    }
    const bool selected = next < count && (!haveOld || hashes[next] <= oldKey);
    const uint64_t key = selected ? hashes[next++] : oldKey;
    uint16_t mask = 0;
    if (haveOld && key == oldKey) {
      mask = oldMask;
      haveOld = false;
    }
    if (selected) mask = (mask & ~touched) | (enabled & touched);
    mask &= activeMask();
    if (!mask) continue;
    if (written == MAX_MEMBERS) return false;
    uint8_t bytes[10];
    for (int i = 0; i < 8; ++i) bytes[i] = uint8_t(key >> (8 * i));
    bytes[8] = uint8_t(mask);
    bytes[9] = uint8_t(mask >> 8);
    if (output.write(bytes, sizeof(bytes)) != sizeof(bytes)) return false;
    hash = digest(hash, bytes, sizeof(bytes));
    ++written;
  }
  header[4] = uint8_t(written);
  header[5] = uint8_t(written >> 8);
  const uint8_t tail[] = {uint8_t(hash), uint8_t(hash >> 8), uint8_t(hash >> 16), uint8_t(hash >> 24)};
  if (output.write(tail, 4) != 4 || !output.seek(0) || output.write(header, 6) != 6 || !output.close()) return false;
  // Close the old reader before rotating files, for SD/Windows compatibility.
  if (input && !input.close()) return false;
  if (Storage.exists(kBackup) && !Storage.remove(kBackup)) return false;
  const bool hadOld = Storage.exists(kPath);
  if (hadOld && !Storage.rename(kPath, kBackup)) return false;
  if (!Storage.rename(kTemp, kPath)) {
    if (hadOld) Storage.rename(kBackup, kPath);
    return false;
  }
  if (hadOld) Storage.remove(kBackup);
  members = written;
  return true;
}
bool LibraryCategories::selectionMasks(const uint64_t* hashes, size_t count, uint16_t& any, uint16_t& all) const {
  if (!ready || !sortedUnique(hashes, count)) return false;
  any = 0;
  all = activeMask();
  HalFile input;
  if (members && (!Storage.openFileForRead("LIBCAT", kPath, input) || !input.seek(kHeaderBytes))) return false;
  size_t next = 0;
  for (uint16_t i = 0; i < members; ++i) {
    uint64_t key;
    uint16_t mask;
    if (!readMember(input, key, mask)) return false;
    while (next < count && hashes[next] < key) {
      all = 0;
      ++next;
    }
    if (next < count && hashes[next] == key) {
      any |= mask;
      all &= mask;
      ++next;
    }
  }
  if (next < count) all = 0;
  return true;
}
bool LibraryCategories::loadHashes(int slot, std::unique_ptr<uint64_t[]>& hashes, size_t& count) const {
  hashes.reset();
  count = 0;
  if (!ready || slot < -1 || slot >= MAX_CATEGORIES) return false;
  if (!members) return true;
  HalFile input;
  if (!Storage.openFileForRead("LIBCAT", kPath, input) || !input.seek(kHeaderBytes)) return false;
  size_t needed = 0;
  for (uint16_t i = 0; i < members; ++i) {
    uint64_t key;
    uint16_t mask;
    if (!readMember(input, key, mask)) return false;
    if (slot == -1 || (mask & (1u << slot))) ++needed;
  }
  if (!needed) return true;
  // Eight bytes per match (max 32 KB), only while applying a category filter.
  // Two sequential metadata passes avoid allocating for unrelated categories.
  auto result = makeUniqueNoThrow<uint64_t[]>(needed);
  if (!result) {
    LOG_ERR("LIBCAT", "filter allocation failed (%u books)", static_cast<unsigned>(needed));
    return false;
  }
  if (!input.seek(kHeaderBytes)) return false;
  for (uint16_t i = 0; i < members; ++i) {
    uint64_t key;
    uint16_t mask;
    if (!readMember(input, key, mask)) return false;
    if (slot == -1 || (mask & (1u << slot))) result[count++] = key;
  }
  hashes = std::move(result);
  return true;
}
}  // namespace library
