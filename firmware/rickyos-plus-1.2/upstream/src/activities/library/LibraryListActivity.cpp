#include "LibraryListActivity.h"

#include <Bitmap.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIncoming.h>
#include <LibrarySources.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <RickyPerf.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "LibraryCategoriesActivity.h"
#include "LibraryFoldersActivity.h"
#include "LibrarySourcesActivity.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/home/RickyStorageActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyDesignUi.h"
#include "components/RickyPageUi.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/controls/button.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/rickyPageIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookCoverLoader.h"
#include "util/PanelTileCache.h"

namespace fui = freeink::ui;

namespace {
// Recent books that vanished are pruned at most every ten minutes, and after transfers.
unsigned long lastRecentPrune = 0;
constexpr int SIDE_PADDING = 12;
constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Three short tab labels: a full-slot pill would stretch across a third of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
}

void LibraryListActivity::onEnter() {
  // Publish one coherent cached page. No scan/EPUB parsing runs under this lock.
  RenderLock lock(*this);
  const unsigned long enterStarted = millis();
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);
  app.on(
      ACTION_ADD,
      [](const fui::ActionEvent& event, void* ctx) {
        auto& self = *static_cast<LibraryListActivity*>(ctx);
        // Six handler slots: row/tab + search/back + tools/filter. Multiplex
        // toolbar values instead of silently overflowing the fixed host table.
        if (event.value == BatchSelect)
          self.batchAction();
        else if (event.value == SortTools)
          self.openShelfTools();
        else if (event.value == ManageCategories) {
          // In batch mode this line reads "N selected · tap to cancel".
          if (self.batchMode) {
            RenderLock lock(self);
            self.clearBatch();
            self.requestUpdate();
          } else {
            self.openCategories();
          }
        }
        else if (!self.batchMode && event.value == ManageSources)
          self.openSources();
        else if (!self.batchMode && event.value == ManageFolders)
          self.openFolders();
        else if (event.value == PageJump)
          self.promptPageJump();
      },
      this);
  const unsigned long tCategories = millis();
  categoriesReady = categories.load() != library::LibraryCategories::Load::Error;
  const unsigned long tCategoriesDone = millis();
  app.on(
      ACTION_READING_FILTER,
      [](const fui::ActionEvent& event, void* user) {
        auto& self = *static_cast<LibraryListActivity*>(user);
        if (event.value < 0 || event.value > 2) return;
        if (self.batchMode) return;
        RenderLock lock(self);
        self.readingFilter = event.value;
        self.applyFilter();
        self.activeNav().reset();
        self.app.clearTapFlash();
        self.requestUpdate();
      },
      this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  const unsigned long tPrune = millis();
  // Checking every recent book on the card is ~230 ms; books rarely vanish between
  // two visits, so it runs on the first visit, after transfers and every ten minutes.
  if (lastRecentPrune == 0 || millis() - lastRecentPrune > 10UL * 60 * 1000 || library::LibraryIncoming::pending()) {
    lastRecentPrune = millis() | 1;
    if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  }
  const unsigned long tIndex = millis();

  // Dirty means an explicit update is available, not permission to walk the
  // card on entry. A retained catalog remains usable while imports are pending.
  // Nothing enrolled yet: an index left by an earlier whole-card scan is not the
  // Library, which starts empty until the reader adds folders or books.
  if (library::LibrarySources::enrolled()) index.open(library::libraryIndexPath());
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  const unsigned long tPinned = millis();
  resolvePinned();
  const unsigned long tPinnedDone = millis();
  // A shelf an earlier build carried over from before sources: rebuild from the
  // enrolled sources only (the import retires the snapshot first).
  if (index.isOpen() && Storage.exists(library::LibrarySources::legacyIndexPath()))
    library::LibraryIncoming::markUnknownChanges();
  transferCheckPending = library::LibraryIncoming::pending();
  transferCheckAt = millis() + 400;  // let the shelf paint first

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  LOG_INF("LIB", "enter: base %lu, categories %lu, recents check %lu, index open %lu, recent rows %lu, total %lu ms",
          tCategories - enterStarted, tCategoriesDone - tCategories, tIndex - tPrune, tPinned - tIndex,
          tPinnedDone - tPinned, millis() - enterStarted);
  requestUpdate(true);
}

void LibraryListActivity::loop() {
  // After the first paint: the import screen replaces the shelf, never the other way round.
  if (transferCheckPending && static_cast<long>(millis() - transferCheckAt) >= 0) foldInTransfers();
  UiTabListActivity::loop();
  if (coverPending && !batchMode && millis() - coverIdleSince > 1500) makePendingCover();
}

void LibraryListActivity::makePendingCover() {
  std::string path;
  int height = 0;
  bool last = false;
  {
    RenderLock lock(*this);
    if (!coverPending || coverPageStart != shelfStart) {
      coverPending = 0;
      return;
    }
    const int slot = __builtin_ctz(coverPending);
    coverPending &= coverPending - 1;
    coverAttempted |= 1u << slot;
    if (slot >= shelfCount) return;
    path = shelfPaths[slot];
    height = coverPageHeight;
    last = coverPending == 0;
  }
  // One book per loop pass: input queued meanwhile is handled before the next.
  // Like Home's covers: the render task is held off and the decode borrows the
  // framebuffer, which is repainted afterwards anyway.
  [[maybe_unused]] const auto started = millis();
  bool generated = false;
  bool made = false;
  {
    RenderLock lock(*this);
    GfxRenderer::FrameBufferLoan loan(renderer);
    made = !BookCoverLoader::ensureThumbnail(path, height, &generated).empty();
  }
  LOG_INF("LIB", "cover %s in %lu ms: %s", made ? (generated ? "made" : "found") : "unavailable", millis() - started,
          path.c_str());
  if (last) {
    // One repaint for the page: the loans cleared the framebuffer, and every cover
    // that could be made is now on the card.
    RenderLock lock(*this);
    shelfStart = -1;
    requestUpdate();
  }
}

void LibraryListActivity::foldInTransfers() {
  transferCheckPending = false;
  auto sources = makeUniqueNoThrow<library::LibrarySources>();
  if (!sources) return;
  const auto loaded = sources->load();
  if (loaded == library::LibrarySources::Load::Error) return;  // keep the record for a later visit
  if (loaded == library::LibrarySources::Load::Missing || sources->size() == 0) {
    library::LibraryIncoming::clear();  // nothing enrolled: transfers are not shelf books yet
    return;
  }
  struct Coverage {
    const library::LibrarySources* sources;
    std::array<uint8_t, library::LibrarySources::MAX_SOURCES / 8> covered{};
  } coverage{sources.get()};
  bool unknownChanges = false;
  library::LibraryIncoming::read(
      [](const char* path, void* context) {
        auto& c = *static_cast<Coverage*>(context);
        for (uint16_t i = 0; i < c.sources->size(); ++i)
          if (c.sources->sourceIncludes(i, path)) c.covered[i / 8] |= uint8_t(1u << (i % 8));
      },
      &coverage, unknownChanges);
  int coveredCount = 0, onlySource = -1;
  for (uint16_t i = 0; i < sources->size(); ++i)
    if (coverage.covered[i / 8] & (1u << (i % 8))) {
      ++coveredCount;
      onlySource = i;
    }
  if (!unknownChanges && coveredCount == 0) {
    // Uploaded outside every enrolled folder: the reader adds those explicitly.
    library::LibraryIncoming::clear();
    return;
  }
  LibraryImportRequest request;
  if (!unknownChanges && coveredCount == 1) {
    request.action = LibraryImportRequest::Action::RefreshSource;
    request.source = static_cast<uint16_t>(onlySource);
  }
  LOG_INF("LIB", "transfers: refresh %s (sources covered=%d, usb=%d)",
          request.action == LibraryImportRequest::Action::RefreshSource ? "one source" : "all sources", coveredCount,
          unknownChanges ? 1 : 0);
  {
    RenderLock lock(*this);
    index.close();
    searchTable.release();
    app.clearTapFlash();
  }
  if (!startActivityForResultWith<LibraryImportActivity>(
          [this](const ActivityResult& result) {
            if (!result.isCancelled) library::LibraryIncoming::clear();
            reloadIndex();
          },
          request))
    reloadIndex();
}

void LibraryListActivity::onExit() {
  index.close();
  searchTable.release();
  filtered.reset();
  Activity::onExit();
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (readingFilter != 0 || categoryFilter != 0 || !folderPath.empty()) return 0;
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

namespace {
// The pinned rows depend only on the index and the recent-books list. Matching
// them costs a pass over every record plus a file open per recent book (~240 ms
// for 3,000 books), so the last answer is kept for as long as neither changed.
struct PinnedRowsCache {
  bool valid = false;
  uint64_t key = 0;
  uint8_t total = 0;
  uint16_t ascRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t storeRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
} pinnedRowsCache;

uint64_t pinnedRowsKey(const library::LibraryIndexFile& index, const std::vector<RecentBook>& books) {
  uint64_t hash = 14695981039346656037ULL;
  const auto mix = [&hash](const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
  };
  const auto& head = index.header();
  mix(&head, sizeof(head));  // sizes, counts and section offsets change with every rebuild
  for (const auto& book : books) mix(book.path.data(), book.path.size() + 1);
  return hash;
}
}  // namespace

void LibraryListActivity::resolvePinned() {
  shelfStart = -1;
  const auto& books = RECENT_BOOKS.getBooks();
  const uint64_t key = index.isOpen() ? pinnedRowsKey(index, books) : 0;
  if (index.isOpen() && pinnedRowsCache.valid && pinnedRowsCache.key == key) {
    pinnedTotal = pinnedRowsCache.total;
    std::copy(std::begin(pinnedRowsCache.ascRows), std::end(pinnedRowsCache.ascRows), pinnedAscRows);
    std::copy(std::begin(pinnedRowsCache.storeRows), std::end(pinnedRowsCache.storeRows), pinnedStoreRows);
    refreshOverlap();
    return;
  }
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; ++i) pinnedStoreRows[i] = i;
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  // Only enrolled books belong to Library. Opening a file directly from
  // Storage must not enroll it through the recent-books overlay.
  uint8_t included = 0;
  for (uint8_t i = 0; i < pinnedTotal; ++i) {
    if (pinnedAscRows[i] == 0xFFFF) continue;
    pinnedAscRows[included] = pinnedAscRows[i];
    pinnedStoreRows[included++] = i;
  }
  pinnedTotal = included;
  if (index.isOpen()) {
    pinnedRowsCache.valid = true;
    pinnedRowsCache.key = key;
    pinnedRowsCache.total = pinnedTotal;
    std::copy(std::begin(pinnedAscRows), std::end(pinnedAscRows), pinnedRowsCache.ascRows);
    std::copy(std::begin(pinnedStoreRows), std::end(pinnedStoreRows), pinnedRowsCache.storeRows);
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[pinnedStoreRows[selectedEntry()]].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (batchMode) {
    if (index < 0 || index >= bookRowCount() || index >= library::CLIX_MAX_RECORDS) return;
    RenderLock lock(*this);
    const uint8_t bit = uint8_t(1u << (index % 8));
    const bool selected = batchSelected[index / 8] & bit;
    batchSelected[index / 8] ^= bit;
    batchCount += selected ? -1 : 1;
    app.clearTapFlash();
    requestUpdate();
    return;
  }
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (!query.empty() || !groupable()); }

void LibraryListActivity::onRowLongPress(const int index) {
  if (batchMode) {
    activateIndex(index);
    return;
  }
  if (UITheme::getInstance().hasMainTabs() && !groupsCollapsed) {
    showRecentBookOptions(index);
  } else if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[pinnedStoreRows[entry]].path;
    title = books[pinnedStoreRows[entry]].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_LIBRARY_REMOVE_BOOK), tr(STR_DELETE),
                                 tr(STR_LIBRARY_ASSIGN_CATEGORY), tr(STR_REMOVE_FROM_RECENTS)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_LIBRARY_REMOVE_BOOK), tr(STR_DELETE),
                                 tr(STR_LIBRARY_ASSIGN_CATEGORY)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 5 : 4, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         removeFromLibrary(path);
                         break;
                       case 2:
                         promptDeleteBookByPath(path, title);
                         break;
                       case 3:
                         assignCategories(path);
                         break;
                       case 4:
                         if (isStoreRow) promptRemoveRecentBook(path, title);
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Explicit updates have their own cancellable activity, never a blocking popup.
void LibraryListActivity::promptRebuildIndex() {
  {
    RenderLock lock(*this);
    index.close();
    app.clearTapFlash();
  }
  if (!startActivityForResultWith<LibraryImportActivity>([this](const ActivityResult&) { reloadIndex(); },
                                                         LibraryImportRequest{}))
    reloadIndex();
}

void LibraryListActivity::reloadIndex() {
  RenderLock lock(*this);
  index.close();
  searchTable.release();
  searchTableFailed = false;
  if (library::LibrarySources::enrolled()) index.open(library::libraryIndexPath());
  degraded = index.isOpen() && index.ranksDegraded();
  resetAfterRebuild();
  swallowHeldReleases();
  requestUpdate();
}
void LibraryListActivity::openSources() {
  {
    RenderLock lock(*this);
    index.close();
    app.clearTapFlash();
  }
  if (!startActivityForResultWith<LibrarySourcesActivity>([this](const ActivityResult&) { reloadIndex(); }))
    reloadIndex();
}
void LibraryListActivity::removeFromLibrary(const std::string& path) {
  {
    RenderLock lock(*this);
    index.close();
    app.clearTapFlash();
  }
  if (!startActivityForResultWith<LibraryImportActivity>(
          [this](const ActivityResult&) { reloadIndex(); },
          LibraryImportRequest{LibraryImportRequest::Action::ExcludeBook, path}))
    reloadIndex();
}

void LibraryListActivity::toggleListView() {
  RenderLock lock(*this);
  // Keep the same book in view: the new page is the one that contains it.
  SETTINGS.libraryListView = SETTINGS.libraryListView ? 0 : 1;
  SETTINGS.saveToFile();
  shelfStart = -1;
  requestUpdate();
}

void LibraryListActivity::promptPageJump() {
  if (batchMode || pageCapacity <= 0) return;
  const int count = bookRowCount();
  const int pages = (count + pageCapacity - 1) / pageCapacity;
  if (pages <= 1) return;
  app.clearTapFlash();
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        swallowHeldReleases();
        if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;
        const int page = std::atoi(std::get<KeyboardResult>(result.data).text.c_str());
        RenderLock lock(*this);
        const int total = bookRowCount();
        const int last = (total + pageCapacity - 1) / pageCapacity;
        if (page < 1 || total <= 0) return;
        const int row = (std::min(page, last) - 1) * pageCapacity;
        // Ring position 0 is the filter strip; book rows start at 1.
        activeNav().requestSelection(row + 1);
        shelfStart = -1;
        requestUpdate();
      },
      tr(STR_LIBRARY_JUMP_PAGE), std::string(), size_t(5), InputType::Latin);
}

void LibraryListActivity::clearBatch() {
  batchMode = false;
  batchCount = 0;
  batchSelected.fill(0);
  app.clearTapFlash();
}
void LibraryListActivity::openShelfTools() {
  if (batchMode) {
    batchAction();
    return;
  }
  RenderLock lock(*this);
  app.clearTapFlash();
  static constexpr StrId options[] = {StrId::STR_RICKY_SORT_RECENT_DESC, StrId::STR_RICKY_SORT_RECENT_ASC,
                                      StrId::STR_RICKY_SORT_TITLE_ASC,   StrId::STR_RICKY_SORT_TITLE_DESC,
                                      StrId::STR_RICKY_SORT_AUTHOR_ASC,  StrId::STR_RICKY_SORT_AUTHOR_DESC,
                                      StrId::STR_LIBRARY_REBUILD,        StrId::STR_LIBRARY_CATEGORIES,
                                      StrId::STR_LIBRARY_BATCH,          StrId::STR_LIBRARY_SOURCES,
                                      StrId::STR_LIBRARY_VIEW_TOGGLE,    StrId::STR_LIBRARY_FOLDERS};
  const int selected = activeTabIndex * 2 + (isDescending(sortOrder) == (activeTabIndex == RECENT_TAB) ? 0 : 1);
  optionPopup.show(StrId::STR_RICKY_LIBRARY_TOOLS, options, std::size(options), selected, [this](int choice) {
    swallowHeldReleases();
    if (choice == 6) {
      promptRebuildIndex();
      return;
    }
    if (choice == 7) {
      openCategories();
      return;
    }
    if (choice == 8) {
      batchAction();
      return;
    }
    if (choice == 9) {
      openSources();
      return;
    }
    if (choice == 10) {
      toggleListView();
      return;
    }
    if (choice == 11) {
      openFolders();
      return;
    }
    if (choice < 0 || choice >= 6 || degraded) return;
    RenderLock lock(*this);
    const int tab = choice / 2;
    const bool descending = tab == RECENT_TAB ? choice % 2 == 0 : choice % 2 != 0;
    if (descending)
      descendingTabs |= static_cast<uint8_t>(1u << tab);
    else
      descendingTabs &= static_cast<uint8_t>(~(1u << tab));
    selectTab(tab, false);
  });
  requestUpdate();
}
void LibraryListActivity::batchAction() {
  if (batchMode) {
    if (batchCount) assignCategories();
    return;
  }
  RenderLock lock(*this);
  clearBatch();
  batchMode = true;
  requestUpdate();
}
void LibraryListActivity::openCategories() {
  app.clearTapFlash();
  startActivityForResultWith<LibraryCategoriesActivity>([this](const ActivityResult& result) {
    RenderLock lock(*this);
    categoriesReady = categories.load() != library::LibraryCategories::Load::Error;
    if (!result.isCancelled && std::holds_alternative<IntervalResult>(result.data))
      categoryFilter = std::get<IntervalResult>(result.data).value;
    if (categoryFilter < 0 || categoryFilter > 17 || (categoryFilter >= 2 && !*categories.name(categoryFilter - 2)))
      categoryFilter = 0;
    applyFilter();
    activeNav().reset();
    swallowHeldReleases();
    requestUpdate();
  });
}
void LibraryListActivity::openFolders() {
  std::vector<LibraryFoldersActivity::Folder> folders;
  {
    RenderLock lock(*this);
    app.clearTapFlash();
    std::vector<std::string> paths;
    std::vector<uint16_t> counts;
    if (index.isOpen() && index.readFolderPaths(paths) && index.countBooksPerFolder(counts) &&
        paths.size() == counts.size()) {
      folders.reserve(paths.size());
      for (size_t id = 0; id < paths.size(); ++id)
        if (counts[id]) folders.push_back({std::move(paths[id]), counts[id]});
    } else {
      LOG_ERR("LIB", "cannot list library folders");
    }
  }
  startActivityForResultWith<LibraryFoldersActivity>(
      [this](const ActivityResult& result) {
        RenderLock lock(*this);
        if (!result.isCancelled && std::holds_alternative<FilePathResult>(result.data))
          folderPath = std::get<FilePathResult>(result.data).path;
        applyFilter();
        activeNav().reset();
        swallowHeldReleases();
        requestUpdate();
      },
      std::move(folders), folderPath);
}

void LibraryListActivity::assignCategories(const std::string& path) {
  const size_t count = path.empty() ? batchCount : 1;
  if (!count) return;
  // At most 32 KB, only for the user's explicit batch transaction, released
  // on leaving the child. No second vector of paths or physical book reads.
  auto hashes = makeUniqueNoThrow<uint64_t[]>(count);
  bool ok = bool(hashes);
  size_t written = 0;
  {
    RenderLock lock(*this);
    if (ok && !path.empty())
      hashes[written++] = library::clixPathHash(path.data(), path.size());
    else if (ok) {
      std::string bookPath;
      for (int entry = 0; entry < bookRowCount(); ++entry) {
        if (!(batchSelected[entry / 8] & (1u << (entry % 8)))) continue;
        if (written >= count || !entryPath(entry, bookPath)) {
          ok = false;
          break;
        }
        hashes[written++] = library::clixPathHash(bookPath.data(), bookPath.size());
      }
    }
    if (!ok || written != count) {
      LOG_ERR("LIBCAT", "cannot prepare category selection (%u books)", static_cast<unsigned>(count));
      const char* choices[] = {tr(STR_DONE)};
      optionPopup.showMessage(tr(STR_LIBRARY_CATEGORIES), tr(STR_LIBRARY_CATEGORY_ERROR), choices, 1, 0, [](int) {});
      requestUpdate();
      return;
    }
    std::sort(hashes.get(), hashes.get() + written);
    written = std::unique(hashes.get(), hashes.get() + written) - hashes.get();
    app.clearTapFlash();
  }
  startActivityForResultWith<LibraryCategoriesActivity>(
      [this](const ActivityResult& result) {
        RenderLock lock(*this);
        categoriesReady = categories.load() != library::LibraryCategories::Load::Error;
        if (!result.isCancelled) clearBatch();
        if (categoryFilter >= 2 && !*categories.name(categoryFilter - 2)) categoryFilter = 0;
        // A cancelled batch retains its selection and order. No membership edits
        // are committed until Save; names created in the child remain available.
        if (!batchMode) applyFilter();
        swallowHeldReleases();
        requestUpdate();
      },
      std::move(hashes), written);
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        clearBookCache(path);
        if (!Storage.remove(path.c_str())) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        RickyStorageActivity::invalidateScan();
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    }
    if (!result.isCancelled) {
      promptRebuildIndex();
    }
  });
}

void LibraryListActivity::openSearch() {
  if (batchMode) return;
  app.clearTapFlash();
  // Chinese characters, pinyin initials ("jtyss") or Latin words all go through
  // the same keyboard; LibrarySearchTable decides how each word matches.
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        swallowHeldReleases();
        if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;
        RenderLock lock(*this);
        query = std::get<KeyboardResult>(result.data).text;
        applyFilter();
        auto& nav = activeNav();
        nav.selected = 0;
        nav.top = 0;
        requestUpdate();
      },
      tr(STR_LIBRARY_SEARCH), query, size_t(64), InputType::Text);
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (batchMode) return;
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (readingFilter != 0 || categoryFilter != 0 || !folderPath.empty()) return filteredCount;
  if (!query.empty()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (!query.empty() || readingFilter != 0 || categoryFilter != 0 || !folderPath.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const {
  return false;  // RickyOS shelves navigate into books, never fold/unfold groups.
}

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches: from LibrarySearchTable in
// memory when it could be built, otherwise straight from the index file. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  shelfStart = -1;
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty() && readingFilter == 0 && categoryFilter == 0 && folderPath.empty()) return;

  std::unique_ptr<uint64_t[]> categoryHashes;
  size_t categoryHashCount = 0;
  if (categoryFilter &&
      (!categoriesReady || !categories.loadHashes(categoryFilter - 2, categoryHashes, categoryHashCount))) {
    filterFailed = true;
    return;
  }

  const std::string needle = library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;
  folderMatch.clear();
  if (!folderPath.empty()) {
    // One byte per folder: the folder itself and everything under it.
    std::vector<std::string> paths;
    if (!index.readFolderPaths(paths)) {
      filterFailed = true;
      return;
    }
    folderMatch.assign(paths.size(), 0);
    for (size_t id = 0; id < paths.size(); ++id) {
      const std::string& p = paths[id];
      folderMatch[id] = p.size() >= folderPath.size() && p.compare(0, folderPath.size(), folderPath) == 0 &&
                        (p.size() == folderPath.size() || p[folderPath.size()] == '/');
    }
  }
  const auto inFolder = [this](const uint16_t folderId) {
    return folderPath.empty() || (folderId < folderMatch.size() && folderMatch[folderId]);
  };
  if ((!query.empty() || categoryFilter) && !searchTable.ready() && !searchTableFailed && index.isOpen())
    searchTableFailed = !searchTable.build(index);
  const bool inMemory = searchTable.ready() && searchTable.size() == total;
  const auto prepared = LibrarySearchTable::prepare(query);
  [[maybe_unused]] const auto filterStarted = millis();

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    if (inMemory) {
      // Title, author, initials and category from memory; the card is read only
      // for a reading-state filter, and only for books that already matched.
      const uint16_t ordinal = searchTable.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
      if (ordinal == 0xFFFF || !searchTable.matches(ordinal, prepared)) continue;
      if (categoryFilter) {
        const bool present = categoryHashCount && std::binary_search(categoryHashes.get(),
                                                                     categoryHashes.get() + categoryHashCount,
                                                                     searchTable.pathHash(ordinal));
        if (categoryFilter == 1 ? present : !present) continue;
      }
      if (!folderPath.empty()) {
        library::ClixRecord record{};
        if (!index.readRecord(ordinal, record) || !inFolder(record.folderId)) continue;
      }
      if (readingFilter != 0) {
        library::ClixRecord record{};
        std::string path;
        if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) continue;
        const auto* stats = READING_STATS.findBook(path);
        const bool started = stats != nullptr;
        const bool reading = started && !stats->completed;
        if ((readingFilter == 1 && !reading) || (readingFilter == 2 && started)) continue;
      }
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (!inFolder(record.folderId)) continue;
    if (categoryFilter) {
      uint64_t hash = 0;
      if (!index.readPathHash(record, hash)) {
        filterFailed = true;
        return;
      }
      const bool present =
          categoryHashCount && std::binary_search(categoryHashes.get(), categoryHashes.get() + categoryHashCount, hash);
      if (categoryFilter == 1 ? present : !present) continue;
    }
    if (readingFilter != 0) {
      std::string path;
      if (!index.readPath(record, path)) continue;
      // Shelf filtering must not content-hash every unread file. Path aliases
      // are already resident; content reconciliation belongs to book opening.
      const auto* stats = READING_STATS.findBook(path);
      const bool started = stats != nullptr;
      const bool reading = started && !stats->completed;
      if ((readingFilter == 1 && !reading) || (readingFilter == 2 && started)) continue;
    }
    if (query.empty()) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
  LOG_INF("LIB", "filter \"%s\" reading=%u category=%d folder=\"%s\": %u of %d books in %lu ms (%s)", query.c_str(),
          static_cast<unsigned>(readingFilter), categoryFilter, folderPath.c_str(), static_cast<unsigned>(matchCount),
          total,
          millis() - filterStarted, inMemory ? "memory" : "card");
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  if (batchMode) {
    RenderLock lock(*this);
    clearBatch();
    requestUpdate();
    return;
  }
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[pinnedStoreRows[entry]];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

// The shelf pages four books at a time. The generic list scrolls by the rows the last
// page drew, which on a last page holding one book is one row; the shelf start rounds
// up to its page, so swiping back from there never left the last page. Scrolling stays
// in UiListActivity::loop(), after the touch is routed: taking the swipe earlier left
// the touched book pressed (drawn inverted) because its release never reached the UI.
int LibraryListActivity::swipeScrollRows() { return pageCapacity; }

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (batchMode) {
      batchAction();  // physical-button path to assigning a cross-page batch
      return true;
    }
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      if (batchMode)
        batchAction();
      else
        stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  auto& nav = activeNav();
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused()) openShelfTools();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count, &nav] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count, &nav] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  fui::header(screen.frame(),
              fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding), frameRect.width,
                        static_cast<int16_t>(metrics.headerHeight)},
              header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  buildRickyShelf(screen);
  return;
}

bool LibraryListActivity::entryPath(int entry, std::string& path) {
  if (entry < 0 || entry >= bookRowCount()) return false;
  if (entry < pinnedCount()) {
    path = RECENT_BOOKS.getBooks()[pinnedStoreRows[entry]].path;
    return true;
  }
  const auto ordinal = index.ordinalForRow(sortOrder, rowFor(entry));
  library::ClixRecord record{};
  return ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readPath(record, path);
}

void LibraryListActivity::buildRickyShelf(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_LIBRARY));
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), static_cast<int16_t>(bounds.x),
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height),
                  static_cast<int16_t>(renderer.getScreenWidth() - bounds.x - bounds.width)});
  screen.insetContent(fui::Insets{0, ricky::ds::space::kPageMargin, 0, ricky::ds::space::kPageMargin});
  const int gap = ricky::ds::space::kGutter;
  const int labelHeight = screen.target().lineHeight(theme.smallText.font);
  // The product caption can fall back to BODY for a title with uncommon glyphs.
  // Reserve the taller of the two actual font lines so title and progress never touch.
  const int cardTextLineHeight =
      std::max({renderer.getLineHeight(RickyDesignUi::CAPTION), renderer.getLineHeight(RickyDesignUi::BODY),
                renderer.getLineHeight(RickyDesignUi::BOOK_TITLE)});
  constexpr int cardInset = 8;
  constexpr int cardTextGap = 8;
  constexpr int cardSideInset = 6;
#if (defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4) || defined(SIMULATOR_DEVICE_EEGO_A4)
  // The A4's shelf rows are short: a title line only, so the covers can grow (reading
  // state stays in the filter chips and the cover's progress bar).
  constexpr int cardTextLines = 1;
#else
  constexpr int cardTextLines = 2;
#endif
  const int cardExtraHeight = cardTextLines * (cardTextLineHeight + cardTextGap) + 2 * cardInset;
  const int count = bookRowCount();
  auto titleRect = screen.takeTop(ricky::ds::layout::kPageHeaderHeight, 0);
  auto countRect = titleRect;
  auto addRect = titleRect;
  addRect.width = ricky::ds::layout::kTouchMin;
  addRect.x = titleRect.right() - addRect.width;
  auto addStyle = theme.titleText;
  addStyle.align = fui::TextAlign::Center;
  screen.target().text(addRect, "+", addStyle);
  screen.frame().hit(addRect, ACTION_ADD, 0, fui::InputTouch);
  // Search sits beside "+": with thousands of books it is the main way in.
  auto searchRect = addRect;
  searchRect.x = addRect.x - searchRect.width;
  RickyPageUi::pageIcon(screen.target(), renderer, searchRect.x + (searchRect.width - 40) / 2,
                        searchRect.y + searchRect.height / 2, icon_ricky_lookup_40);
  screen.frame().hit(searchRect, ACTION_SEARCH, 0, fui::InputTouch);
  countRect.width = titleRect.width / 3;
  countRect.x = searchRect.x - gap - countRect.width;
  titleRect.width -= countRect.width + addRect.width + searchRect.width + gap * 2;
  auto pageTitle = theme.titleText;
  pageTitle.bold = true;
  screen.target().text(titleRect, headerTitle(), pageTitle);
  char position[48];
  snprintf(position, sizeof(position), tr(STR_RICKY_LIBRARY_COUNT), static_cast<unsigned>(count));
  auto countStyle = theme.smallText;
  countStyle.color = fui::Color::DarkGray;
  countStyle.align = fui::TextAlign::Right;
  screen.target().text(countRect, position, countStyle);
  auto filters = screen.takeTop(ricky::ds::layout::kTouchMin, gap);
  // Sort/refresh remains the existing menu; the shared icon has an 88 px hit.
  const int moreWidth = std::max(44, filters.width / 6);
  auto moreRect = filters;
  moreRect.x += filters.width - moreWidth;
  moreRect.width = moreWidth;
  RickyPageUi::pageIcon(screen.target(), renderer, moreRect.x + (moreRect.width - 40) / 2,
                        moreRect.y + moreRect.height / 2, icon_ricky_filter_40);
  screen.frame().hit(moreRect, ACTION_ADD, SortTools, fui::InputTouch);
  const StrId filterLabels[] = {StrId::STR_RICKY_BOOKS_ALL, StrId::STR_RICKY_BOOKS_READING,
                                StrId::STR_RICKY_BOOKS_UNREAD};
  int widestLabel = 0;
  for (auto id : filterLabels)
    widestLabel = std::max(
        widestLabel,
        static_cast<int>(screen.target().measureText(theme.smallText.font, I18N.get(id), theme.smallText).width));
  const int width = std::min((moreRect.x - filters.x - gap * 3) / 3, std::max(44, widestLabel + gap * 3));
  for (int i = 0; i < 3; ++i) {
    auto rect = filters;
    rect.x += i * (width + gap);
    rect.width = width;
    screen.frame().hit(rect, ACTION_READING_FILTER, i, fui::InputTouch);
    auto labelRect = rect;
    labelRect.height = ricky::ds::layout::kTabPillHeight;
    labelRect.y += (filters.height - labelRect.height) / 2;
    const bool picked = readingFilter == i;
    if (picked)
      screen.target().fill(labelRect, fui::Paint::solid(fui::Color::Black), labelRect.height / 2);
    else
      screen.target().stroke(labelRect, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kHairline,
                             labelRect.height / 2);
    auto label = theme.smallText;
    label.bold = false;
    label.color = picked ? fui::Color::White : fui::Color::Black;
    label.align = fui::TextAlign::Center;
    screen.target().text(labelRect, I18N.get(filterLabels[i]), label);
  }
  // Category and folder are independent of reading status: two outline chips sized
  // to their names, the same pill as the reading filters above. A chip that narrows
  // the shelf is filled, so a filtered shelf is visible at a glance.
  auto categoryRow = screen.takeTop(ricky::ds::layout::kTouchMin, gap);
  auto batchRect = categoryRow;
  batchRect.width = std::max(44, categoryRow.width / 5);
  batchRect.x = categoryRow.right() - batchRect.width;
  auto caption = theme.smallText;
  caption.color = fui::Color::DarkGray;
  char selectedText[64];
  snprintf(selectedText, sizeof(selectedText), tr(STR_LIBRARY_BATCH_SELECTED), static_cast<unsigned>(batchCount));
  const char* categoryName = categoryFilter == 0   ? tr(STR_LIBRARY_ALL_CATEGORIES)
                             : categoryFilter == 1 ? tr(STR_LIBRARY_UNCATEGORIZED)
                                                   : categories.name(categoryFilter - 2);
  if (batchMode) {
    // In batch mode this line reads "N selected"; tapping it cancels the batch.
    auto selectedRect = categoryRow;
    selectedRect.width = batchRect.x - gap - categoryRow.x;
    screen.target().text(selectedRect, selectedText, caption);
    screen.frame().hit(selectedRect, ACTION_ADD, ManageCategories, fui::InputTouch);
  } else {
    // The chip names the folder itself; the picker shows the full path.
    const size_t slash = folderPath.rfind('/');
    const std::string folderName =
        folderPath.empty() ? std::string(tr(STR_LIBRARY_ALL_FOLDERS))
                           : (slash == std::string::npos ? folderPath : folderPath.substr(slash + 1));
    const int chipsRight = batchRect.x - gap;
    int x = categoryRow.x;
    const auto chip = [&](const char* text, const bool active, const int maxWidth, const int action) {
      auto label = theme.smallText;
      label.bold = false;
      label.align = fui::TextAlign::Center;
      label.color = active ? fui::Color::White : fui::Color::Black;
      const int natural = screen.target().measureText(label.font, text, label).width + gap * 3;
      auto hit = categoryRow;
      hit.x = x;
      hit.width = std::max(44, std::min(natural, maxWidth));
      auto pill = hit;
      pill.height = ricky::ds::layout::kTabPillHeight;
      pill.y += (categoryRow.height - pill.height) / 2;
      if (active)
        screen.target().fill(pill, fui::Paint::solid(fui::Color::Black), pill.height / 2);
      else
        screen.target().stroke(pill, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kHairline,
                               pill.height / 2);
      screen.target().text(pill, text, label);
      screen.frame().hit(hit, ACTION_ADD, action, fui::InputTouch);
      x = hit.right() + gap;
    };
    chip(categoryName, categoryFilter != 0, (chipsRight - categoryRow.x - gap) / 2, ManageCategories);
    chip(folderName.c_str(), !folderPath.empty(), chipsRight - x, ManageFolders);
  }
  caption.align = fui::TextAlign::Right;
  screen.target().text(batchRect, batchMode ? tr(STR_LIBRARY_ASSIGN_CATEGORY) : tr(STR_LIBRARY_BATCH), caption);
  screen.frame().hit(batchRect, ACTION_ADD, BatchSelect, fui::InputTouch);
  if (count == 0) {
    screen.centeredText(filterFailed ? tr(STR_LIBRARY_SEARCH_UNAVAILABLE)
                                     : (query.empty() && readingFilter == 0 && categoryFilter == 0 && folderPath.empty()
                                            ? tr(STR_LIBRARY_NOT_ENROLLED)
                                            : tr(STR_LIBRARY_NO_RESULTS)));
    return;
  }
  const bool listView = SETTINGS.libraryListView != 0;
  const bool multiplePages = count > SHELF_CAPACITY || (listView && count > 1);
  const auto positionRect = multiplePages ? screen.takeBottom(labelHeight, gap) : fui::Rect{};
  const auto body = screen.body();
  // List rows: title over "author · progress", sized from the real font lines.
  const int listRowHeight = cardTextLineHeight * 2 + cardTextGap + 2 * cardInset;
  const int capacity =
      listView ? std::clamp(body.height / std::max(1, listRowHeight), 4, SHELF_MAX) : SHELF_CAPACITY;
  pageCapacity = capacity;
  // One row only on a clearly wide body; a near-square one (the A4: about 480 x 420 in
  // portrait) keeps two rows, or its covers shrink to a sliver of the width.
  const int columns = listView ? 1 : (body.width > body.height * 3 / 2 ? 2 : 1) * (SHELF_CAPACITY / 2);
  const int rows = capacity / columns;
  const int rowHeight = std::max(1, (body.height - gap * (rows - 1)) / rows);
  // 2:3 covers, one page of six bounded records.
  const int coverHeight =
      listView ? 0
               : std::max(1, std::min(rowHeight - cardExtraHeight,
                                      ((body.width - gap * (columns - 1)) / columns - 16) * 3 / 2));
  auto& n = activeNav();
  fui::ListProps paging;
  // Feed the existing atomic list-nav requests with virtual one-pixel rows.
  // No new input owner or second set of manually overlapping touch rectangles.
  n.syncToProps(fui::Rect{0, 0, 1, static_cast<int16_t>(capacity)}, 1, 0, count, paging, 1);
  const int start = n.followPending && paging.selectedIndex >= 0
                        ? paging.selectedIndex / capacity * capacity
                        : std::min((count - 1) / capacity * capacity,
                                   (paging.topIndex + capacity - 1) / capacity * capacity);
  const int visible = std::min(capacity, count - start);
  LOG_DBG("RICKY", "shelf bounds=%d,%d,%d,%d body=%d,%d,%d,%d start=%d count=%d visible=%d cover=%d list=%d",
          bounds.x, bounds.y, bounds.width, bounds.height, body.x, body.y, body.width, body.height, start, count,
          visible, coverHeight, listView ? 1 : 0);
  if (start != shelfStart || coverHeight != shelfHeight || capacity != shelfBuiltCapacity) {
    [[maybe_unused]] const auto perfShelfStart = millis();
    shelfCount = visible;
    // Materialize only one bounded page of index records; no vector of every book/cover.
    for (int i = 0; i < visible; ++i) {
      rowTextFor(start + i, shelfTitles[i], shelfAuthors[i]);
      RickyPageUi::stripBookExtension(shelfTitles[i]);
      if (!entryPath(start + i, shelfPaths[i])) shelfPaths[i].clear();
      const auto& path = shelfPaths[i];
      // Reading state only: "EPUB · 已读 12%" overflowed a three-column card, and the
      // format already shows on generated covers.
      const auto* stats = READING_STATS.findBook(path);
      if (stats && stats->lastProgressPercent >= 100)
        snprintf(shelfDetails[i].data(), shelfDetails[i].size(), "%s", tr(STR_RICKY_BOOK_FINISHED));
      else if (stats)
        snprintf(shelfDetails[i].data(), shelfDetails[i].size(), tr(STR_RICKY_BOOK_PROGRESS),
                 static_cast<unsigned>(stats->lastProgressPercent));
      else
        snprintf(shelfDetails[i].data(), shelfDetails[i].size(), "%s", tr(STR_RICKY_BOOK_UNREAD));
      shelfCovers[i].clear();
    }
    [[maybe_unused]] const auto perfThumbStart = millis();
    // Look up only retained thumbnail names. Painting a page must never unzip
    // EPUBs or create images; the existing title-cover design is the fallback.
    for (int i = 0; i < visible && !listView; ++i) {
      if (shelfPaths[i].empty()) continue;
      // A cover already drawn this session was a valid thumbnail: skip the card.
      std::string drawn = BookCoverLoader::thumbnailPath(shelfPaths[i], coverHeight);
      shelfCovers[i] = std::move(drawn);  // cached-only; missing files use the designed title cover
    }
    RICKY_PERF("shelf", "start=%d records=%lu thumbs=%lu", start, perfThumbStart - perfShelfStart,
               millis() - perfThumbStart);
    LOG_INF("LIB", "page %d: records %lu, covers %lu ms", start, perfThumbStart - perfShelfStart,
            millis() - perfThumbStart);
    shelfStart = start;
    shelfHeight = coverHeight;
    shelfBuiltCapacity = capacity;
  }
  if (listView) {
    const int lineHeight = std::max(1, (body.height) / capacity);
    std::string detail;
    for (int slot = 0; slot < visible; ++slot) {
      const int entry = start + slot;
      const fui::Rect cell{body.x, static_cast<int16_t>(body.y + slot * lineHeight), body.width,
                           static_cast<int16_t>(lineHeight)};
      const bool picked = batchMode ? (batchSelected[entry / 8] & (1u << (entry % 8))) != 0
                                    : showMainTabContentSelection() && paging.selectedIndex == entry;
      if (picked) RickyDesignUi::fill(renderer, cell, ricky::ds::gray::kInk, ricky::ds::radius::kControl);
      const int textHeight = cardTextLineHeight * 2 + cardTextGap;
      const fui::Rect titleRect{static_cast<int16_t>(cell.x + cardSideInset * 2),
                                static_cast<int16_t>(cell.y + (cell.height - textHeight) / 2),
                                static_cast<int16_t>(cell.width - cardSideInset * 4),
                                static_cast<int16_t>(cardTextLineHeight)};
      const fui::Rect detailRect{titleRect.x, static_cast<int16_t>(titleRect.bottom() + cardTextGap), titleRect.width,
                                 titleRect.height};
      RickyDesignUi::bookTitle(renderer, titleRect, shelfTitles[slot].c_str(), RickyDesignUi::BOOK_TITLE, picked,
                          fui::TextAlign::Left);
      detail = shelfAuthors[slot].empty() ? std::string(shelfDetails[slot].data())
                                          : shelfAuthors[slot] + " · " + shelfDetails[slot].data();
      RickyDesignUi::secondaryText(renderer, detailRect, detail.c_str(), RickyDesignUi::CAPTION, picked,
                                   fui::TextAlign::Left);
      if (slot + 1 < visible && !picked)
        screen.target().fill(fui::Rect{titleRect.x, static_cast<int16_t>(cell.bottom() - 1), titleRect.width, 1},
                             fui::Paint::solid(fui::Color::LightGray));
      screen.frame().hit(cell, ACTION_ROW, entry, fui::InputTouch | fui::InputLongPress);
    }
  }
  if (start != coverPageStart || coverHeight != coverPageHeight) {
    coverPageStart = start;
    coverPageHeight = coverHeight;
    coverAttempted = 0;
  }
  coverPending = 0;
  coverIdleSince = millis();
  const int cellWidth = (body.width - gap * (columns - 1)) / columns;
  for (int slot = 0; slot < visible && !listView; ++slot) {
    const fui::Rect cell{static_cast<int16_t>(body.x + slot % columns * (cellWidth + gap)),
                         static_cast<int16_t>(body.y + slot / columns * (rowHeight + gap)),
                         static_cast<int16_t>(cellWidth), static_cast<int16_t>(coverHeight + cardExtraHeight)};
    const int entry = start + slot;
    const bool picked = batchMode ? (batchSelected[entry / 8] & (1u << (entry % 8))) != 0
                                  : showMainTabContentSelection() && paging.selectedIndex == entry;
    if (picked) RickyDesignUi::fill(renderer, cell, ricky::ds::gray::kInk, ricky::ds::radius::kControl);
    const int coverWidth = coverHeight * 2 / 3;
    const fui::Rect rect{static_cast<int16_t>(cell.x + (cell.width - coverWidth) / 2),
                         static_cast<int16_t>(cell.y + cardInset),
                         static_cast<int16_t>(coverWidth), static_cast<int16_t>(coverHeight)};
    bool painted = false;
    if (!shelfCovers[slot].empty()) {
      // 1-bit covers leave paper pixels undrawn, so the cell fill under them is part of the key.
      char key[48];
      snprintf(key, sizeof(key), "|lib|%d,%d,%d,%d|%d", rect.x, rect.y, rect.width, rect.height, picked ? 1 : 0);
      painted = PanelTileCache::draw(renderer, shelfCovers[slot] + key, shelfCovers[slot], rect.x, rect.y, rect.width,
                                     rect.height, [&] {
                                       HalFile file;
                                       if (!Storage.openFileForRead("LIB", shelfCovers[slot], file)) return false;
                                       Bitmap bitmap(file);
                                       return bitmap.parseHeaders() == BmpReaderError::Ok &&
                                              GUI.drawCoverThumbFill(renderer, bitmap,
                                                                     Rect{rect.x, rect.y, rect.width, rect.height});
                                     });
      if (painted) screen.target().stroke(rect, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kHairline);
    }
    if (!painted && !shelfPaths[slot].empty() && !(coverAttempted & (1u << slot)) &&
        BookCoverLoader::canMakeThumbnail(shelfPaths[slot], coverHeight))
      coverPending |= 1u << slot;
    if (!painted) {
      const auto& path = shelfPaths[slot];
      const char* format = FsHelpers::hasEpubExtension(path) ? "EPUB" : FsHelpers::hasTxtExtension(path) ? "TXT" :
                           FsHelpers::checkFileExtension(path, ".mobi") ? "MOBI" :
                           FsHelpers::checkFileExtension(path, ".azw3") ? "AZW3" : "MD";
      RickyDesignUi::cover(renderer, rect, shelfTitles[slot].c_str(), format);
    }
    const fui::Rect titleRect{static_cast<int16_t>(cell.x + cardSideInset),
                              static_cast<int16_t>(rect.bottom() + cardTextGap),
                              static_cast<int16_t>(cell.width - cardSideInset * 2),
                              static_cast<int16_t>(cardTextLineHeight)};
    const fui::Rect detailRect{titleRect.x, static_cast<int16_t>(titleRect.bottom() + cardTextGap), titleRect.width,
                               titleRect.height};
    // Medium has BODY's full repertoire and line box, so book titles stay at one
    // size/weight while progress retains its smaller secondary face.
    RickyDesignUi::bookTitle(renderer, titleRect, shelfTitles[slot].c_str(), RickyDesignUi::BOOK_TITLE, picked,
                        fui::TextAlign::Center);
    if (cardTextLines > 1) {
      RickyDesignUi::secondaryText(renderer, detailRect, shelfDetails[slot].data(), RickyDesignUi::CAPTION, picked,
                                   fui::TextAlign::Center);
    }
    screen.frame().hit(cell, ACTION_ROW, start + slot, fui::InputTouch | fui::InputLongPress);
  }
  n.drawnCount = count;
  n.onListRendered(start, visible, paging.selectedIndex >= start && paging.selectedIndex < start + visible);
  snprintf(position, sizeof(position), tr(STR_RICKY_LIBRARY_PAGE), static_cast<unsigned>(start / capacity + 1),
           static_cast<unsigned>((count + capacity - 1) / capacity), static_cast<unsigned>(count));
  auto style = theme.smallText;
  style.align = fui::TextAlign::Right;
  if (multiplePages) {
    screen.target().text(positionRect, position, style);
    // The page readout jumps: hundreds of pages are too many to turn one by one.
    screen.frame().hit(positionRect, ACTION_ADD, PageJump, fui::InputTouch);
  }
  GUI.drawSideScrollBar(renderer, Rect{body.x, body.y, body.width, body.height}, count, start, capacity);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() / 2 - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.isActive()) renderPopupUnderlay();
  if (optionPopup.processRender(renderer, mappedInput)) return;
  const unsigned long started = millis();
  UiTabListActivity::render(std::move(lock));
  LOG_INF("LIB", "render %lu ms (panel included)", millis() - started);
}

void LibraryListActivity::drawFooter() {
  const auto productLabels = mainTabButtonLabels(tr(STR_BACK), tr(STR_OPEN), listCount() > 1);
  GUI.drawButtonHints(renderer, productLabels.btn1, productLabels.btn2, productLabels.btn3, productLabels.btn4);
  return;
}
