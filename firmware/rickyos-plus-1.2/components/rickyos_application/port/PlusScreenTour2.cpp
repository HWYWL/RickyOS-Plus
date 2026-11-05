// Diagnostic only (RICKYOS_PLUS_SCREEN_TOUR=2): marketing screenshots of the user's own
// books, with the user's state put back afterwards. Before the tour the files reading and
// standby touch are copied to /.crosspoint/tour-backup with their CRC; settings are only
// changed in RAM. The next boot copies everything back (or removes what did not exist),
// checks each CRC, removes the backup and restarts so RAM loads the restored files.
#if RICKYOS_PLUS_SCREEN_TOUR == 2
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "RecentBooksStore.h"

namespace {
constexpr char kDir[] = "/.crosspoint/tour-backup";
constexpr char kManifest[] = "/.crosspoint/tour-backup/manifest.txt";
constexpr char kDone[] = "/.crosspoint/tour-backup-done";
constexpr char kSticker[] = "/standby.png";
constexpr char kTear[] = "/images/透明壁纸/破纸而出.png";

uint32_t crcOf(const std::string& path, bool& ok) {
  ok = false;
  HalFile file;
  if (!Storage.openFileForRead("TOUR", path, file)) return 0;
  uint8_t buffer[1024];
  uint32_t crc = 0xffffffffu;
  for (;;) {
    const int count = file.read(buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0) return 0;
    for (int i = 0; i < count; ++i) {
      crc ^= buffer[i];
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
  }
  ok = true;
  return crc ^ 0xffffffffu;
}

bool copyFile(const std::string& from, const std::string& to) {
  HalFile in, out;
  if (!Storage.openFileForRead("TOUR", from, in) || !Storage.openFileForWrite("TOUR", to, out)) return false;
  uint8_t buffer[1024];
  for (;;) {
    const int count = in.read(buffer, sizeof(buffer));
    if (count == 0) return true;
    if (count < 0 || out.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) return false;
  }
}

std::string progressOf(const std::string& path) { return Epub(path, "/.crosspoint").getCachePath() + "/progress.bin"; }
}  // namespace

std::string plusTour2Book(const char* titlePart) {
  for (const auto& book : RECENT_BOOKS.getBooks())
    if (book.title.find(titlePart) != std::string::npos || book.path.find(titlePart) != std::string::npos)
      return book.path;
  return "";
}

bool plusTour2Backup(const std::string& republic, const std::string& dance) {
  if (Storage.exists(kManifest)) return false;
  Storage.mkdir(kDir);
  std::vector<std::string> paths = {"/.crosspoint/settings.json", "/.crosspoint/settings.bin", "/.crosspoint/state.json",
                                    "/.crosspoint/state.bin", "/.crosspoint/recent.json",
                                    "/.crosspoint/reading_stats.json", kSticker};
  if (!republic.empty()) paths.push_back(progressOf(republic));
  if (!dance.empty()) paths.push_back(progressOf(dance));
  std::string manifest;
  for (size_t i = 0; i < paths.size(); ++i) {
    const auto& path = paths[i];
    const bool exists = Storage.exists(path.c_str());
    bool ok = !exists;
    uint32_t crc = 0;
    if (exists) {
      const std::string copy = std::string(kDir) + "/" + std::to_string(i) + ".bak";
      bool readOk = false, copyOk = copyFile(path, copy);
      crc = crcOf(path, readOk);
      bool backupOk = false;
      ok = copyOk && readOk && crcOf(copy, backupOk) == crc && backupOk;
    }
    char line[32];
    snprintf(line, sizeof(line), "%u %d %08lx ", static_cast<unsigned>(i), exists ? 1 : 0, static_cast<unsigned long>(crc));
    manifest += line + path + "\n";
    LOG_INF("TOUR2", "backup %s exists=%d crc=%08lx ok=%d", path.c_str(), exists, static_cast<unsigned long>(crc), ok);
    if (!ok) return false;
  }
  HalFile out;
  return Storage.openFileForWrite("TOUR", kManifest, out) &&
         out.write(manifest.data(), manifest.size()) == manifest.size();
}

// Returns true when a backup was restored (the caller restarts).
bool plusTour2Restore() {
  if (!Storage.exists(kManifest)) return false;
  std::string text;
  {
    HalFile in;
    if (!Storage.openFileForRead("TOUR", kManifest, in)) return false;
    char buffer[512];
    for (int count; (count = in.read(buffer, sizeof(buffer))) > 0;) text.append(buffer, count);
  }
  unsigned restored = 0, bad = 0;
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    at = end == std::string::npos ? text.size() : end + 1;
    unsigned index = 0, existed = 0;
    unsigned long crc = 0;
    int consumed = 0;
    if (sscanf(line.c_str(), "%u %u %lx %n", &index, &existed, &crc, &consumed) < 3 || consumed <= 0) continue;
    const std::string path = line.substr(static_cast<size_t>(consumed));
    const std::string copy = std::string(kDir) + "/" + std::to_string(index) + ".bak";
    bool ok;
    if (existed) {
      bool readOk = false;
      ok = copyFile(copy, path) && crcOf(path, readOk) == crc && readOk;
      if (ok) Storage.remove(copy.c_str());
    } else {
      ok = !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
    }
    LOG_INF("TOUR2", "restore %s existed=%u crc=%08lx ok=%d", path.c_str(), existed, crc, ok);
    ok ? ++restored : ++bad;
  }
  if (bad == 0) {
    Storage.remove(kManifest);
    Storage.rmdir(kDir);
    HalFile done;
    Storage.openFileForWrite("TOUR", kDone, done);
  }
  LOG_INF("TOUR2", "restore done restored=%u bad=%u", restored, bad);
  return true;
}

// After the restart that follows a restore: the tour must not start again.
bool plusTour2Finished() {
  if (!Storage.exists(kDone)) return false;
  Storage.remove(kDone);
  return true;
}

int plusTour2FindChapter(const std::string& path, const char* needle) {
  Epub epub(path, "/.crosspoint");
  if (!epub.load(true, true)) return -1;
  for (int i = 0; i < epub.getSpineItemsCount(); ++i) {
    size_t size = 0;
    uint8_t* bytes = epub.readItemContentsToBytes(epub.getSpineItem(i).href, &size, true);
    const bool found = bytes && strstr(reinterpret_cast<const char*>(bytes), needle) != nullptr;
    free(bytes);
    if (found) return i;
  }
  return -1;
}

bool plusTour2SetProgress(const std::string& path, int spine) {
  HalFile out;
  const uint8_t data[4] = {static_cast<uint8_t>(spine), static_cast<uint8_t>(spine >> 8), 0, 0};
  return Storage.openFileForWrite("TOUR", progressOf(path), out) && out.write(data, sizeof(data)) == sizeof(data);
}

bool plusTour2InstallSticker() { return Storage.exists(kTear) && copyFile(kTear, kSticker); }
#endif
