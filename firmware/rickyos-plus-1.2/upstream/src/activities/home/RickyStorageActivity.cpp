#include "RickyStorageActivity.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#include "CrossPointSettings.h"
#include "FileBrowserActivity.h"
#include "activities/ActivityManager.h"
#include "activities/library/LibrarySourcesActivity.h"
#include <LibrarySources.h>
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
#include "activities/settings/FontLibraryActivity.h"
#include "components/RickyPageUi.h"
#include "components/UITheme.h"
#include "components/icons/rickyPageIcons.h"
#include "util/RickyFreeSpace.h"
#include "util/RickyStorageLayout.h"

void RickyStorageActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_RICKY_STORAGE));
}
void RickyStorageActivity::drawFooter() {
  const auto labels = mainTabButtonLabels(tr(STR_BACK), tr(STR_OPEN), true);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
namespace {
// "43.1 GB" / "820 MB": one decimal below 100 GB, whole numbers above.
void formatBytes(char* out, size_t size, uint64_t bytes) {
  constexpr uint64_t MB = 1024ULL * 1024ULL, GB = MB * 1024ULL;
  if (bytes >= 100 * GB)
    snprintf(out, size, "%u GB", static_cast<unsigned>((bytes + GB / 2) / GB));
  else if (bytes >= GB)
    snprintf(out, size, "%u.%u GB", static_cast<unsigned>(bytes / GB), static_cast<unsigned>(bytes % GB * 10 / GB));
  else
    snprintf(out, size, "%u MB", static_cast<unsigned>((bytes + MB / 2) / MB));
}

// Set when the page is left: a folder walk stops between entries and leaves the counts
// marked stale, so the next visit finishes them instead of the walk competing with a book.
std::atomic<bool> scanCancel{false};
}  // namespace

const char* RickyStorageActivity::folderFor(const int index) {
  static constexpr const char* folders[] = {RickyStorageLayout::BOOKS, RickyStorageLayout::FONTS,
                                            RickyStorageLayout::IMAGES, RickyStorageLayout::DOWNLOADS};
  return index >= 0 && index < 4 ? folders[index] : "/";
}

// Numbers come from the cheapest source that is right, each shown as soon as it lands:
//  - total and free space: the volume (RickyFreeSpace, ~1 s on a 32 GB card, lock per slice);
//  - books: the Library index header, the very number Library shows; a folder walk only
//    while there is no fresh index;
//  - fonts: the font registry, in memory;
//  - images and downloads: a folder walk, lock per entry.
// The last numbers are saved on the card, so a visit after a restart shows them at once.
// Counts are redone only when something marked them stale (a transfer, a delete, ...; the
// mark is a file too, so it survives a restart) or on Refresh. Free space also drifts with
// reader caches, so a visit refreshes it once it is a few minutes old.
namespace {
struct StorageScan {
  bool measured = false;
  bool attempted = false;
  uint64_t total = 0;
  uint64_t free = 0;
  std::array<int, 3> counts{-1, -1, -1};  // books, images, downloads
  bool sized = false;
  uint64_t bookBytes = 0, cacheBytes = 0, fontBytes = 0;
};
constexpr const char* kStatsPath = "/.crosspoint/storage-stats.json";
// "1" = counts out of date. Rewritten, never deleted: this page removes nothing on the card.
constexpr const char* kStalePath = "/.crosspoint/storage-stats.stale";
constexpr uint32_t kFreeSpaceMaxAgeMs = 3 * 60 * 1000;

std::mutex scanMutex;
StorageScan lastScan;                     // guarded by scanMutex
bool haveScan = false;                    // guarded by scanMutex: lastScan holds real numbers
bool freeMeasured = false;                // guarded by scanMutex: measured since boot
uint32_t freeMeasuredMs = 0;              // guarded by scanMutex
std::atomic<uint32_t> scanGeneration{0};  // bumped on every result the page should show
std::atomic<bool> scanRunning{false};
std::atomic<bool> countsWanted{false};
std::atomic<bool> countsStale{false};  // mirrored by kStalePath
std::atomic<uint32_t> staleEpoch{0};   // invalidations; a count pass clears only its own epoch
bool loadedFromCard = false;           // loop task only

template <typename Update>
void publish(Update&& update) {
  {
    std::lock_guard<std::mutex> lock(scanMutex);
    update(lastScan);
    haveScan = true;
  }
  scanGeneration.fetch_add(1);
}

void saveStats() {
  StorageScan scan;
  {
    std::lock_guard<std::mutex> lock(scanMutex);
    scan = lastScan;
  }
  JsonDocument doc;
  doc["total"] = scan.total;
  doc["free"] = scan.free;
  doc["attempted"] = scan.attempted;
  doc["books"] = scan.counts[0];
  doc["images"] = scan.counts[1];
  doc["downloads"] = scan.counts[2];
  doc["sized"] = scan.sized;
  doc["bookBytes"] = scan.bookBytes;
  doc["cacheBytes"] = scan.cacheBytes;
  doc["fontBytes"] = scan.fontBytes;
  String text;
  serializeJson(doc, text);
  if (!Storage.writeFile(kStatsPath, text)) LOG_ERR("STOR", "Cannot save storage stats");
}

// Once per boot, on the first visit: a few hundred bytes, no walk.
void loadStats() {
  if (loadedFromCard) return;
  loadedFromCard = true;
  if (Storage.readFile(kStalePath) == "1") countsStale.store(true);
  const String text = Storage.readFile(kStatsPath);
  JsonDocument doc;
  if (text.isEmpty() || deserializeJson(doc, text) != DeserializationError::Ok) {
    countsStale.store(true);  // never counted on this card
    return;
  }
  publish([&](StorageScan& scan) {
    scan.total = doc["total"] | static_cast<uint64_t>(0);
    scan.free = doc["free"] | static_cast<uint64_t>(0);
    scan.measured = scan.total > 0 && scan.free <= scan.total;
    scan.attempted = doc["attempted"] | scan.measured;
    scan.counts = {doc["books"] | -1, doc["images"] | -1, doc["downloads"] | -1};
    scan.sized = doc["sized"] | false;
    scan.bookBytes = doc["bookBytes"] | static_cast<uint64_t>(0);
    scan.cacheBytes = doc["cacheBytes"] | static_cast<uint64_t>(0);
    scan.fontBytes = doc["fontBytes"] | static_cast<uint64_t>(0);
  });
}

// Books are whatever Library counts: its index header (a few ms). A folder walk of our
// own would apply other rules (formats Library does not open, reader records, duplicates)
// and show a number Library then contradicts. -1 while the index is missing or marked
// dirty (a transfer): the last number stays up until Library or Refresh rebuilds it.
// Loop task only: Library rebuilds the index there too, so the two never overlap.
int indexBookCount() {
  if (library::isLibraryIndexDirty()) return -1;
  auto index = makeUniqueNoThrow<library::LibraryIndexFile>();
  if (!index || !index->open(library::libraryIndexPath())) return -1;
  const int books = index->bookCount();
  index->close();
  return books;
}

// Bytes under `path` (hidden entries included: reader caches live in /.crosspoint).
uint64_t sumBytes(const std::string& path, const int depth = 0) {
  constexpr int kMaxDepth = 4;
  HalFile dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) return 0;
  uint64_t bytes = 0;
  char name[256];
  for (HalFile entry = dir.openNextFile(); entry && !scanCancel.load(); entry = dir.openNextFile()) {
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    const bool folder = entry.isDirectory();
    const uint64_t size = folder ? 0 : entry.fileSize();
    entry.close();
    if (folder) {
      if (depth < kMaxDepth) bytes += sumBytes(path == "/" ? std::string("/") + name : path + "/" + name, depth + 1);
    } else {
      bytes += size;
    }
  }
  dir.close();
  return bytes;
}

// The Library's books by their recorded sizes: one sequential read of the index, no walk.
uint64_t libraryBytes() {
  auto index = makeUniqueNoThrow<library::LibraryIndexFile>();
  if (!index || !index->open(library::libraryIndexPath())) return 0;
  uint64_t bytes = 0;
  library::ClixRecord records[32];
  const uint16_t count = index->bookCount();
  for (uint32_t first = 0; first < count && !scanCancel.load(); first += 32) {
    const uint32_t n = std::min<uint32_t>(32, count - first);
    if (!index->readRange(library::recordOffset(index->header(), static_cast<uint16_t>(first)), records,
                          n * sizeof(library::ClixRecord)))
      break;
    for (uint32_t i = 0; i < n; ++i) bytes += records[i].fileSize;
  }
  index->close();
  return bytes;
}

void countPass() {
  const uint32_t epoch = staleEpoch.load();
  const uint32_t started = millis();
  // Books are not walked: their number and sizes are Library's index.
  const uint64_t books = libraryBytes();
  const uint64_t cache = scanCancel.load() ? 0 : sumBytes("/.crosspoint");
  const uint64_t fonts = scanCancel.load() ? 0 : sumBytes(RickyStorageLayout::FONTS) + sumBytes("/.fonts");
  if (scanCancel.load()) {
    LOG_INF("STOR", "Counting paused after %u ms", static_cast<unsigned>(millis() - started));
    return;  // stays stale: the next visit counts again
  }
  publish([&](StorageScan& scan) {
    scan.sized = true;
    scan.bookBytes = books;
    scan.cacheBytes = cache;
    scan.fontBytes = fonts;
  });
  LOG_INF("STOR", "Counted files in %u ms", static_cast<unsigned>(millis() - started));
  if (staleEpoch.load() == epoch && countsStale.exchange(false)) Storage.writeFile(kStalePath, "0");
}

void scanStorageTask(void*) {
  uint64_t total = 0, free = 0;
  const bool measured = RickyFreeSpace::measure(total, free);
  publish([&](StorageScan& scan) {
    scan.measured = measured;
    scan.attempted = true;
    scan.total = total;
    scan.free = free;
  });
  {
    std::lock_guard<std::mutex> lock(scanMutex);
    freeMeasured = measured;
    freeMeasuredMs = millis();
  }
  while (!scanCancel.load() && countsWanted.exchange(false)) countPass();
  saveStats();
  scanRunning.store(false);
  scanGeneration.fetch_add(1);  // the page drops its "counting" state
  vTaskDelete(nullptr);
}

void startStorageScan(const bool counts) {
  if (counts) countsWanted.store(true);
  // Back on the page before a paused pass ended: let it carry on and pick countsWanted up.
  scanCancel.store(false);
  if (scanRunning.exchange(true)) return;
  // sumBytes recurses four folders deep with a 256-byte name buffer per level.
  if (xTaskCreate(&scanStorageTask, "RickyStorageScan", 6144, nullptr, 1, nullptr) != 1) {  // pdPASS
    LOG_ERR("STOR", "Cannot start the storage scan");
    scanRunning.store(false);
  }
}
}  // namespace

void RickyStorageActivity::invalidateScan() {
  staleEpoch.fetch_add(1);
  if (!countsStale.exchange(true)) Storage.writeFile(kStalePath, "1");
}

void RickyStorageActivity::applyLastScan() {
  StorageScan scan;
  bool have;
  {
    std::lock_guard<std::mutex> lock(scanMutex);
    scan = lastScan;
    have = haveScan;
  }
  RenderLock lock(*this);
  seenScans = scanGeneration.load();
  counting = scanRunning.load();
  if (!have) return;  // never measured on this card: keep the placeholders
  space = scan.measured    ? Space::Ready
          : counting       ? Space::Measuring
          : scan.attempted ? Space::Unavailable
                           : Space::NotMeasured;
  sdTotalBytes = scan.total;
  sdFreeBytes = scan.free;
  counts[0] = scan.counts[0];
  counts[2] = scan.counts[1];
  counts[3] = scan.counts[2];
  sized = scan.sized;
  bookBytes = scan.bookBytes;
  cacheBytes = scan.cacheBytes;
  fontBytes = scan.fontBytes;
}

void RickyStorageActivity::refresh() {
  // Storage statistics are explicit, independent work. They never change the
  // user's Library sources or rebuild the catalog.
  const int indexed = indexBookCount();
  if (indexed >= 0) publish([&](StorageScan& scan) { scan.counts[0] = indexed; });
  invalidateScan();
  startStorageScan(true);
  applyLastScan();
  requestUpdate();
}

void RickyStorageActivity::onEnter() {
  UiListActivity::onEnter();
  // Downloaded fonts live in /.fonts and imported ones in /fonts: count the families
  // the font registry found in both, not the visible folder's entries. In memory, cheap.
  counts[1] = static_cast<int>(sdFontSystem.registry().getFamilies().size());
  loadStats();
  countLibraryFolders();
  // Library may have rebuilt its index since the last visit: take its count as it is now.
  const int indexed = indexBookCount();
  if (indexed >= 0) publish([&](StorageScan& scan) { scan.counts[0] = indexed; });
  // Cached summary only on entry. Refresh explicitly requests statistics;
  // stale numbers do not authorize a directory or FAT walk.
  applyLastScan();
  requestUpdate();
}

void RickyStorageActivity::countLibraryFolders() {
  libraryFolders = -1;
  auto sources = makeUniqueNoThrow<library::LibrarySources>();
  if (!sources || sources->load() == library::LibrarySources::Load::Error) return;
  libraryFolders = 0;
  for (uint16_t i = 0; i < sources->size(); ++i)
    if ((*sources)[i].kind == library::SourceKind::Folder || (*sources)[i].kind == library::SourceKind::RecursiveFolder)
      ++libraryFolders;
}

void RickyStorageActivity::onExit() {
  scanCancel.store(true);  // a folder walk pauses; free space (~1 s) finishes on its own
  UiListActivity::onExit();
}

void RickyStorageActivity::loop() {
  if (scanGeneration.load() != seenScans) {
    applyLastScan();
    requestUpdate();
  }
  UiListActivity::loop();
}

void RickyStorageActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto content = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(content.y), static_cast<int16_t>(content.x),
                  static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height),
                  static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width)});
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(fui::Insets{0, ricky::ds::space::kPageMargin, 0, ricky::ds::space::kPageMargin});
  const int gap = ricky::ds::space::kGutter;
  const int bodyHeight = target.lineHeight(theme.bodyText.font);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  RickyPageUi::Bold bold(renderer, 1);
  auto small = theme.smallText;
  small.maxLines = 1;
  auto label = theme.bodyText;
  label.maxLines = 1;
  const int selected = RickyPageUi::syncNav(nav, listCount());
  const bool focus = showMainTabContentSelection();

  auto title = screen.takeTop(ricky::ds::layout::kPageHeaderHeight, 0);
  {
    // Refresh: count everything again (a card edited on a computer leaves no mark).
    const char* refreshLabel = counting ? tr(STR_RICKY_STORAGE_COUNTING) : tr(STR_RICKY_STORAGE_REFRESH);
    const int labelWidth = target.measureText(small.font, refreshLabel, small).width;
    const auto& icon = icon_ricky_refresh_40;
    const int iconGap = gap / 2;
    const int buttonWidth = icon.w + iconGap + labelWidth;
    const int buttonX = title.right() - buttonWidth;
    const int middle = title.y + title.height / 2;
    const fui::Rect hit{static_cast<int16_t>(buttonX - gap), static_cast<int16_t>(title.y - gap / 2),
                        static_cast<int16_t>(buttonWidth + gap * 2), static_cast<int16_t>(title.height + gap)};
    const bool refreshSelected = focus && selected == kRefreshAction;
    if (refreshSelected) RickyPageUi::card(target, hit, true);
    RickyPageUi::pageIcon(target, renderer, buttonX, middle, icon, refreshSelected);
    auto refreshStyle = small;
    refreshStyle.color = refreshSelected ? fui::Color::White : fui::Color::Black;
    target.text(
        fui::Rect{static_cast<int16_t>(buttonX + icon.w + iconGap), static_cast<int16_t>(middle - smallHeight / 2),
                  static_cast<int16_t>(labelWidth + 2), static_cast<int16_t>(smallHeight)},
        refreshLabel, refreshStyle);
    if (!counting) screen.frame().hit(hit, ACTION_ROW, kRefreshAction, fui::InputTouch);
    title.width = static_cast<int16_t>(buttonX - gap - title.x);
  }
  auto pageTitle = theme.titleText;
  pageTitle.bold = true;
  {
    RickyPageUi::Bold heading(renderer, 2);
    target.text(title, tr(STR_RICKY_STORAGE), pageTitle);
  }
  if (folderMissing) {
    auto warning = small;
    warning.maxLines = 2;
    target.text(screen.takeBottom(smallHeight * 2, gap), tr(STR_RICKY_FOLDER_MISSING), warning);
  }

  const int pad = ricky::ds::space::kCardPadding;
  const int summaryHeight = ricky::ds::layout::kProfileCardHeight;
  const int rowHeight = ricky::ds::layout::kSettingsCardHeight;
  const int section = ricky::ds::space::k8;
  char text[64];

  // SD card: display the last statistics; this summary has no action or focus.
  const auto summary = screen.takeTop(summaryHeight, section);
  constexpr bool summarySelected = false;
  RickyPageUi::card(target, summary, summarySelected);
  const fui::Color ink = summarySelected ? fui::Color::White : fui::Color::Black;
  const fui::Color quiet = summarySelected ? fui::Color::White : fui::Color::DarkGray;
  const int innerX = summary.x + pad;
  const int innerWidth = summary.width - pad * 2;
  const int barHeight = 12;
  const int blockHeight = bodyHeight + gap / 2 + barHeight + gap / 2 + smallHeight;
  const int top = summary.y + (summary.height - blockHeight) / 2;
  auto strong = label;
  strong.bold = true;
  strong.color = ink;
  target.text(fui::Rect{static_cast<int16_t>(innerX), static_cast<int16_t>(top), static_cast<int16_t>(innerWidth / 2),
                        static_cast<int16_t>(bodyHeight)},
              tr(STR_RICKY_SD_CARD), strong);
  char freeText[16], totalText[16];
  formatBytes(freeText, sizeof(freeText), sdFreeBytes);
  formatBytes(totalText, sizeof(totalText), sdTotalBytes);
  snprintf(text, sizeof(text), tr(STR_RICKY_STORAGE_SPACE), freeText, totalText);
  auto right = small;
  right.align = fui::TextAlign::Right;
  right.color = quiet;
  target.text(fui::Rect{static_cast<int16_t>(innerX + innerWidth / 2), static_cast<int16_t>(top + bodyHeight - smallHeight),
                        static_cast<int16_t>(innerWidth / 2), static_cast<int16_t>(smallHeight)},
              space == Space::Ready         ? text
              : space == Space::NotMeasured ? tr(STR_RICKY_SPACE_NOT_MEASURED)
              : space == Space::Measuring   ? tr(STR_LOADING)
                                            : tr(STR_NOT_AVAILABLE),
              right);
  // Used space in segments: books, reader caches, then fonts and everything else.
  const fui::Rect bar{static_cast<int16_t>(innerX), static_cast<int16_t>(top + bodyHeight + gap / 2),
                      static_cast<int16_t>(innerWidth), static_cast<int16_t>(barHeight)};
  target.stroke(bar, fui::Paint::solid(ink), 1, barHeight / 2);
  if (space == Space::Ready && sdTotalBytes > 0) {
    const uint64_t used = sdTotalBytes > sdFreeBytes ? sdTotalBytes - sdFreeBytes : 0;
    const uint64_t books = sized ? std::min(bookBytes, used) : 0;
    const uint64_t cache = sized ? std::min(cacheBytes, used - books) : 0;
    const auto widthOf = [&](uint64_t bytes) {
      return static_cast<int>(bytes * static_cast<uint64_t>(bar.width) / sdTotalBytes);
    };
    int x = bar.x;
    const auto segment = [&](int width, fui::Color color) {
      if (width <= 0) return;
      target.fill(fui::Rect{static_cast<int16_t>(x), bar.y, static_cast<int16_t>(std::min<int>(width, bar.right() - x)),
                            bar.height},
                  fui::Paint::solid(color));
      x += width;
    };
    segment(widthOf(books), summarySelected ? fui::Color::White : fui::Color::Black);
    segment(widthOf(cache), summarySelected ? fui::Color::LightGray : fui::Color::DarkGray);
    segment(std::max(widthOf(used) - widthOf(books) - widthOf(cache), used > 0 ? 1 : 0),
            summarySelected ? fui::Color::DarkGray : fui::Color::LightGray);
  }
  char booksText[16], cacheText[16], otherText[16];
  const uint64_t used = sdTotalBytes > sdFreeBytes ? sdTotalBytes - sdFreeBytes : 0;
  formatBytes(booksText, sizeof(booksText), bookBytes);
  formatBytes(cacheText, sizeof(cacheText), cacheBytes);
  formatBytes(otherText, sizeof(otherText),
              used > bookBytes + cacheBytes ? used - bookBytes - cacheBytes : 0);
  snprintf(text, sizeof(text), tr(STR_RICKY_STORAGE_BREAKDOWN), booksText, cacheText, otherText);
  auto legend = small;
  legend.color = quiet;
  target.text(fui::Rect{static_cast<int16_t>(innerX), static_cast<int16_t>(bar.bottom() + gap / 2),
                        static_cast<int16_t>(innerWidth), static_cast<int16_t>(smallHeight)},
              sized && space == Space::Ready ? text : tr(STR_RICKY_STORAGE_TAP_REFRESH), legend);

  // Three ways in: the whole card, the Library's folders, and transfers.
  const StrId names[] = {StrId::STR_RICKY_ALL_FILES, StrId::STR_RICKY_LIBRARY_FOLDERS, StrId::STR_FILE_TRANSFER};
  const freeink::Icon* icons[] = {&icon_ricky_folder_40, &icon_ricky_books_40, &icon_ricky_transfer_40};
  for (int i = 0; i < 3; ++i) {
    const auto rect = screen.takeTop(rowHeight, i == 2 ? 0 : gap);
    const int row = i;
    const bool picked = focus && selected == row;
    RickyPageUi::card(target, rect, picked);
    screen.frame().hit(rect, ACTION_ROW, row, fui::InputTouch);
    const int middle = rect.y + rect.height / 2;
    RickyPageUi::pageIcon(target, renderer, rect.x + pad, middle, *icons[i], picked);
    const int x = rect.x + pad + icons[i]->w + pad;
    const int chevronWidth = 24;
    const int width = rect.right() - x - pad - chevronWidth;
    label.color = picked ? fui::Color::White : fui::Color::Black;
    target.text(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(middle - bodyHeight / 2),
                          static_cast<int16_t>(width), static_cast<int16_t>(bodyHeight)},
                I18N.get(names[i]), label);
    const char* detail = nullptr;
    if (i == 1 && libraryFolders >= 0) {
      snprintf(text, sizeof(text), tr(STR_RICKY_LIBRARY_FOLDER_COUNT), static_cast<unsigned>(libraryFolders));
      detail = text;
    } else if (i == 2) {
      detail = tr(STR_RICKY_TRANSFER_KINDS);
    }
    if (detail) {
      auto detailStyle = small;
      detailStyle.align = fui::TextAlign::Right;
      detailStyle.color = picked ? fui::Color::White : fui::Color::DarkGray;
      target.text(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(middle - smallHeight / 2),
                            static_cast<int16_t>(width), static_cast<int16_t>(smallHeight)},
                  detail, detailStyle);
    }
    RickyPageUi::chevron(target,
                         fui::Rect{static_cast<int16_t>(rect.right() - chevronWidth - pad), rect.y,
                                   static_cast<int16_t>(chevronWidth), rect.height},
                         picked);
  }
}

void RickyStorageActivity::activateIndex(int index) {
  app.clearTapFlash();
  folderMissing = false;
  if (index == kRefreshAction) {
    refresh();
    return;
  }
  if (index == 0) {
    // Back in the opened folder climbs to the card root, then returns here.
    startActivityForResultWith<FileBrowserActivity>(
        [this](const ActivityResult&) {
          if (countsStale.load()) startStorageScan(true);
        },
        "/", FileBrowserActivity::Mode::Books, /*scoped=*/true);
  } else if (index == 1) {
    startActivityForResultWith<LibrarySourcesActivity>([this](const ActivityResult&) {
      countLibraryFolders();
      requestUpdate();
    });
  } else if (index == 2) {
    activityManager.goToFileTransfer();
  }
}
