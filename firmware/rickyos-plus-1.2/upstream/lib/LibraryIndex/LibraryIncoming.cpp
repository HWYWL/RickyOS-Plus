#include "LibraryIncoming.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace library {
namespace {

constexpr char kPath[] = "/.crosspoint/library-incoming.txt";
constexpr char kUnknown[] = "*";  // a line of its own: every source may have changed
constexpr size_t kLineMax = 512;

bool appendLine(const char* line) {
  Storage.mkdir("/.crosspoint");
  HalFile file = Storage.open(kPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || !file.isOpen()) {
    LOG_ERR("LIBIN", "cannot open %s", kPath);
    return false;
  }
  const size_t length = strlen(line);
  const bool ok = file.write(reinterpret_cast<const uint8_t*>(line), length) == length &&
                  file.write(reinterpret_cast<const uint8_t*>("\n"), 1) == 1;
  file.flush();
  return ok;
}

// Counts lines without holding them: the record stays a few kilobytes at most.
size_t lineCount() {
  HalFile file;
  if (!Storage.openFileForRead("LIBIN", kPath, file)) return 0;
  size_t lines = 0;
  uint8_t buffer[256];
  for (int got; (got = file.read(buffer, sizeof(buffer))) > 0;)
    for (int i = 0; i < got; ++i) lines += buffer[i] == '\n';
  return lines;
}

}  // namespace

bool LibraryIncoming::add(const char* path) {
  if (!path || !*path || strlen(path) >= kLineMax || strchr(path, '\n')) return false;
  if (lineCount() >= MAX_PATHS) return markUnknownChanges();
  return appendLine(path);
}

bool LibraryIncoming::markUnknownChanges() { return appendLine(kUnknown); }

bool LibraryIncoming::pending() { return Storage.exists(kPath); }

bool LibraryIncoming::read(void (*visit)(const char*, void*), void* context, bool& unknownChanges) {
  unknownChanges = false;
  HalFile file;
  if (!Storage.openFileForRead("LIBIN", kPath, file)) return false;
  char line[kLineMax];
  size_t used = 0;
  bool overlong = false;
  const auto finish = [&]() {
    line[used] = '\0';
    if (!overlong && used > 0) {
      if (strcmp(line, kUnknown) == 0)
        unknownChanges = true;
      else if (visit)
        visit(line, context);
    }
    used = 0;
    overlong = false;
  };
  uint8_t buffer[256];
  for (int got; (got = file.read(buffer, sizeof(buffer))) > 0;) {
    for (int i = 0; i < got; ++i) {
      if (buffer[i] == '\n') {
        finish();
      } else if (used + 1 < sizeof(line)) {
        line[used++] = static_cast<char>(buffer[i]);
      } else {
        overlong = true;
      }
    }
  }
  finish();
  return true;
}

void LibraryIncoming::clear() {
  if (Storage.exists(kPath) && !Storage.remove(kPath)) LOG_ERR("LIBIN", "cannot clear %s", kPath);
}

}  // namespace library
