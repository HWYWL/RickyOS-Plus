#include "BootPreparation.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalSystem.h>
#endif
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/RenderLock.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"
#include "components/RickyCompanion.h"

namespace BootPreparation {
namespace {
constexpr char kPreparedMarker[] = "/.crosspoint/prepared-version";
// Each line is a panel refresh: often enough to show life, rare enough not to slow the walk.
constexpr unsigned long kStatusEveryMs = 1500;

void firmwareIdentity(char (&identity)[64]) {
#if FREEINK_DEVICE_METALIO_PLUS
  const auto id = HalSystem::getFirmwareBuildId();
  snprintf(identity, sizeof(identity), "%s:%02x%02x%02x%02x%02x%02x%02x%02x", CROSSPOINT_VERSION,
           id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
#else
  snprintf(identity, sizeof(identity), "%s", CROSSPOINT_VERSION);
#endif
}

bool preparedForThisFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("PREP", kPreparedMarker, file)) return false;
  char version[64] = {};
  const int got = file.read(reinterpret_cast<uint8_t*>(version), sizeof(version) - 1);
  char identity[64];
  firmwareIdentity(identity);
  return got > 0 && strcmp(version, identity) == 0;
}

void markPrepared() {
  HalFile file;
  if (!Storage.openFileForWrite("PREP", kPreparedMarker, file)) return;
  char identity[64];
  firmwareIdentity(identity);
  file.write(reinterpret_cast<const uint8_t*>(identity), strlen(identity));
}

bool libraryNeedsBuild() {
  if (library::isLibraryIndexDirty()) return true;
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) return true;
  const bool sameMode = index.header().metadataEnabled == (SETTINGS.libraryUseMetadata != 0);
  index.close();
  return !sameMode;
}

struct Status {
  GfxRenderer& renderer;
  unsigned long shownAt = 0;
  unsigned refreshes = 0;
};

// The dog's lines while the card is walked, a new one every few refreshes so a long
// walk on a big or unsorted card does not sit on one sentence.
const char* sniffLine(const unsigned refreshes) {
  static constexpr StrId kLines[] = {StrId::STR_RICKY_PREP_SNIFF, StrId::STR_RICKY_PREP_SNIFF_FOLDERS,
                                     StrId::STR_RICKY_PREP_WAG};
  const StrId line = kLines[refreshes / 3 % (sizeof(kLines) / sizeof(kLines[0]))];
  if (line == StrId::STR_RICKY_PREP_SNIFF) return RickyCompanion::text(line, StrId::STR_RICKY_PREP_SNIFF_CAT);
  return I18n::getInstance().get(line);
}

// Two centred lines in the lower part of whatever the boot screen shows.
void showStatus(Status& status, const char* title, const char* detail) {
  GfxRenderer& renderer = status.renderer;
  RenderLock lock;
  const int width = renderer.getScreenWidth();
  const int line = renderer.getLineHeight(UI_12_FONT_ID);
  const int top = renderer.getScreenHeight() - 4 * line;
  renderer.fillRect(0, top, width, 3 * line, false);
  renderer.drawCenteredText(UI_12_FONT_ID, top + line / 2, title);
  if (detail && detail[0]) renderer.drawCenteredText(UI_10_FONT_ID, top + line * 3 / 2 + 4, detail);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  status.shownAt = millis();
  ++status.refreshes;
}

void buildLibrary(Status& status) {
  showStatus(status, sniffLine(status.refreshes), "");
  library::setBuildProgress(
      [](const uint16_t books, void* context) {
        auto& status = *static_cast<Status*>(context);
        if (millis() - status.shownAt < kStatusEveryMs) return;
        char detail[48];
        snprintf(detail, sizeof(detail), tr(STR_RICKY_PREP_BOOKS_FOUND), static_cast<unsigned>(books));
        showStatus(status, sniffLine(status.refreshes), detail);
      },
      &status);
  library::BuildStats stats;
  const unsigned long started = millis();
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  library::setBuildProgress(nullptr, nullptr);
  LOG_INF("PREP", "library index %s: %u books in %lu ms", ok ? "built" : "FAILED", static_cast<unsigned>(stats.books),
          millis() - started);
}

// Thumbnails a previous firmware wrote in a format this one replaces (1-bit before gray
// covers): regenerate each at the heights it was made for, so Home shows them at once.
void refreshRecentCovers(Status& status) {
  bool announced = false;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    const size_t marker = book.coverBmpPath.find("thumb_[HEIGHT].bmp");
    if (marker == std::string::npos) continue;
    const std::string folder = book.coverBmpPath.substr(0, marker > 0 ? marker - 1 : 0);
    HalFile dir = Storage.open(folder.c_str());
    if (!dir || !dir.isDirectory()) continue;
    int heights[8];
    int count = 0;
    char name[64];
    for (HalFile entry = dir.openNextFile(); entry && count < 8; entry = dir.openNextFile()) {
      name[0] = '\0';
      entry.getName(name, sizeof(name));
      int height = 0;
      if (!entry.isDirectory() && sscanf(name, "thumb_%d.bmp", &height) == 1 && height > 0) {
        Bitmap bitmap(entry);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && BookCoverLoader::isOutdatedThumbnail(bitmap))
          heights[count++] = height;
      }
      entry.close();
    }
    dir.close();
    for (int i = 0; i < count; ++i) {
      if (!announced) {
        showStatus(status, RickyCompanion::text(StrId::STR_RICKY_PREP_COVERS, StrId::STR_RICKY_PREP_COVERS_CAT), "");
        announced = true;
      }
      BookCoverLoader::ensureThumbnail(book.path, heights[i]);
    }
  }
}
}  // namespace

void run(GfxRenderer& renderer) {
#if defined(RICKYOS_PRODUCT) && RICKYOS_PRODUCT
  // Firmware updates and dirty markers do not authorize whole-card scans.
  (void)renderer;
  return;
#endif
  const bool newFirmware = !preparedForThisFirmware();
  const bool library = libraryNeedsBuild();
  if (!newFirmware && !library) return;
  LOG_INF("PREP", "boot preparation: new firmware %d, library %d", newFirmware ? 1 : 0, library ? 1 : 0);
  Status status{renderer};
  if (library) buildLibrary(status);
  if (newFirmware) {
    refreshRecentCovers(status);
    markPrepared();
  }
}

}  // namespace BootPreparation
