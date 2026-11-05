#include "LibrarySources.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "LibraryBuilder.h"

namespace library {
namespace {
constexpr char kPath[] = "/.crosspoint/library-sources.v1";
constexpr char kTemp[] = "/.crosspoint/library-sources.tmp";
constexpr char kBackup[] = "/.crosspoint/library-sources.bak";
constexpr char kLegacy[] = "/.crosspoint/library-legacy.idx";
uint32_t hashBytes(uint32_t hash, const uint8_t* bytes, size_t count) {
  while (count--) hash = (hash ^ *bytes++) * 16777619u;
  return hash;
}
bool readExact(HalFile& file, void* out, size_t bytes) {
  return file.read(static_cast<uint8_t*>(out), bytes) == static_cast<int>(bytes);
}
}  // namespace

bool LibrarySources::ensureCapacity(uint16_t required) {
  if (required <= capacity) return true;
  if (required > MAX_SOURCES) return false;
  uint16_t next = capacity ? capacity : 8;
  while (next < required) next *= 2;
  // Source paths must outlive list-provider callbacks. A fixed 33 KB object
  // would waste RAM for the usual few folders; unbounded vector growth can
  // abort under -fno-exceptions. This capped array leaves old rules intact on OOM.
  auto grown = makeUniqueNoThrow<LibrarySource[]>(next);
  if (!grown) {
    LOG_ERR("LIBSRC", "source allocation failed (%u rules)", next);
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) grown[i] = sources[i];
  sources = std::move(grown);
  capacity = next;
  return true;
}

bool LibrarySources::validPath(const char* path) {
  if (!path || path[0] != '/') return false;
  const size_t n = strnlen(path, 256);
  if (n == 0 || n >= 256 || (n > 1 && path[n - 1] == '/')) return false;
  for (size_t i = 0; i < n; ++i) {
    if (static_cast<unsigned char>(path[i]) < 32 || path[i] == '\\') return false;
    if (path[i] == '/' && (path[i + 1] == '/' ||
                           (path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == 0 ||
                                                   (path[i + 2] == '.' && (path[i + 3] == '/' || path[i + 3] == 0))))))
      return false;
  }
  return true;
}

LibrarySources::Load LibrarySources::load() {
  count = 0;
  // Recovery after power loss between the two renames. Never guess a scan root
  // on malformed state: the retained shelf remains usable until a retry.
  if (!Storage.exists(kPath) && Storage.exists(kBackup) && !Storage.rename(kBackup, kPath)) return Load::Error;
  if (!Storage.exists(kPath)) return Load::Missing;
  HalFile file;
  if (!Storage.openFileForRead("LIBSRC", kPath, file)) return Load::Error;
  uint8_t header[6]{};
  if (!readExact(file, header, sizeof(header)) || memcmp(header, "LSR1", 4)) return Load::Error;
  const uint16_t total = header[4] | (uint16_t(header[5]) << 8);
  if (total > MAX_SOURCES || !ensureCapacity(total)) return Load::Error;
  uint32_t hash = hashBytes(2166136261u, header, sizeof(header));
  for (uint16_t i = 0; i < total; ++i) {
    uint8_t entry[2];
    if (!readExact(file, entry, 2) || entry[0] < 1 || entry[0] > 4 || entry[1] == 0) return Load::Error;
    auto& source = sources[i];
    source.kind = static_cast<SourceKind>(entry[0]);
    if (!readExact(file, source.path, entry[1])) return Load::Error;
    source.path[entry[1]] = 0;
    if (!validPath(source.path) || strlen(source.path) != entry[1]) return Load::Error;
    hash = hashBytes(hashBytes(hash, entry, 2), reinterpret_cast<uint8_t*>(source.path), entry[1]);
  }
  uint8_t tail[4]{};
  if (!readExact(file, tail, 4) || file.position() != file.fileSize()) return Load::Error;
  const uint32_t stored =
      uint32_t(tail[0]) | (uint32_t(tail[1]) << 8) | (uint32_t(tail[2]) << 16) | (uint32_t(tail[3]) << 24);
  if (stored != hash) return Load::Error;
  count = total;
  return Load::Ready;
}

bool LibrarySources::save(bool keepBackup) const {
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("LIBSRC", kTemp, file)) return false;
  const uint8_t header[] = {'L', 'S', 'R', '1', uint8_t(count), uint8_t(count >> 8)};
  uint32_t hash = hashBytes(2166136261u, header, sizeof(header));
  if (file.write(header, sizeof(header)) != sizeof(header)) return false;
  for (uint16_t i = 0; i < count; ++i) {
    const auto& source = sources[i];
    const uint8_t entry[] = {static_cast<uint8_t>(source.kind), static_cast<uint8_t>(strlen(source.path))};
    if (file.write(entry, 2) != 2 || file.write(reinterpret_cast<const uint8_t*>(source.path), entry[1]) != entry[1])
      return false;
    hash = hashBytes(hashBytes(hash, entry, 2), reinterpret_cast<const uint8_t*>(source.path), entry[1]);
  }
  const uint8_t tail[] = {uint8_t(hash), uint8_t(hash >> 8), uint8_t(hash >> 16), uint8_t(hash >> 24)};
  if (file.write(tail, 4) != 4 || !file.close()) return false;
  if (Storage.exists(kBackup) && !Storage.remove(kBackup)) return false;
  const bool hadOld = Storage.exists(kPath);
  if (hadOld && !Storage.rename(kPath, kBackup)) return false;
  if (!Storage.rename(kTemp, kPath)) {
    if (hadOld) Storage.rename(kBackup, kPath);
    return false;
  }
  if (hadOld && !keepBackup) Storage.remove(kBackup);
  return true;
}

bool LibrarySources::finishUpdate(bool success, bool hadManifest) {
  if (success) return !Storage.exists(kBackup) || Storage.remove(kBackup);
  // Catalog installation failed or was cancelled: undo this edit too. A
  // power loss between remove/rename is recovered by load() from the backup.
  if (hadManifest && !Storage.exists(kBackup)) return false;
  if (Storage.exists(kPath) && !Storage.remove(kPath)) return false;
  return !hadManifest || Storage.rename(kBackup, kPath);
}

void LibrarySources::erase(uint16_t index) {
  if (index >= count) return;
  for (uint16_t i = index + 1; i < count; ++i) sources[i - 1] = sources[i];
  --count;
}
bool LibrarySources::add(const char* path, SourceKind kind) {
  if (!validPath(path)) return false;
  for (uint16_t i = 0; i < count; ++i) {
    if (strcmp(path, sources[i].path)) continue;
    sources[i].kind = kind;
    return true;
  }
  if (count == MAX_SOURCES || !ensureCapacity(count + 1)) return false;
  auto& source = sources[count++];
  source.kind = kind;
  strcpy(source.path, path);
  return true;
}
bool LibrarySources::exclude(const char* path) { return add(path, SourceKind::ExcludedBook); }
bool LibrarySources::excluded(const char* path) const {
  for (uint16_t i = 0; i < count; ++i)
    if (sources[i].kind == SourceKind::ExcludedBook && !strcmp(path, sources[i].path)) return true;
  return false;
}
bool LibrarySources::covers(uint16_t index, const char* path) const {
  const auto& source = sources[index];
  if (source.kind == SourceKind::ExcludedBook) return false;
  if (source.kind == SourceKind::Book) return !strcmp(source.path, path);
  const size_t len = strlen(source.path);
  const bool root = len == 1;
  if (strncmp(source.path, path, len) || (!root && path[len] != '/')) return false;
  const char* name = path + len + (root ? 0 : 1);
  if (!*name) return false;
  if (source.kind == SourceKind::Folder) return !strchr(name, '/');
  int depth = 0;
  for (const char* p = name; *p; ++p)
    if (*p == '/' && ++depth > LIBRARY_MAX_DEPTH) return false;
  return true;
}
bool LibrarySources::sourceIncludes(uint16_t index, const char* path) const {
  return index < count && covers(index, path);
}
bool LibrarySources::accepts(const char* path, uint16_t source) const {
  if (excluded(path)) return false;
  for (uint16_t i = 0; i < source; ++i)
    if (covers(i, path)) return false;
  return true;
}
bool LibrarySources::acceptsLegacy(const char* path) const { return !excluded(path) && accepts(path, count); }
bool LibrarySources::includes(const char* path) const {
  if (excluded(path)) return false;
  for (uint16_t i = 0; i < count; ++i)
    if (covers(i, path)) return true;
  return false;
}
const char* LibrarySources::legacyIndexPath() { return kLegacy; }
bool LibrarySources::retireLegacy() { return !Storage.exists(kLegacy) || Storage.remove(kLegacy); }
bool LibrarySources::enrolled() { return Storage.exists(kPath); }
bool LibrarySources::preserveLegacy() {
  if (Storage.exists(kPath) || Storage.exists(kLegacy)) return true;
  if (!recoverLibraryIndex()) return false;
  if (!Storage.exists(libraryIndexPath())) return true;
  Storage.mkdir("/.crosspoint");
  HalFile input, output;
  constexpr char temp[] = "/.crosspoint/library-legacy.tmp";
  if (!Storage.openFileForRead("LIBSRC", libraryIndexPath(), input) ||
      !Storage.openFileForWrite("LIBSRC", temp, output))
    return false;
  uint8_t buffer[1024];  // bounded copy, not a second resident catalog
  const auto size = input.fileSize();
  while (input.position() < size) {
    const int got = input.read(buffer, sizeof(buffer));
    if (got <= 0 || output.write(buffer, got) != static_cast<size_t>(got)) return false;
  }
  if (!output.close()) return false;
  return Storage.rename(temp, kLegacy);
}
}  // namespace library
