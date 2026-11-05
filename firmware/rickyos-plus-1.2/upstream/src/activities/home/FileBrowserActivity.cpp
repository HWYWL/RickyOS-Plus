#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "InxItemLayout.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/home/RickyStorageActivity.h"
#include "activities/library/LibrarySourcesActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyDesignUi.h"
#include "components/RickyFileLabel.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/inx_library.h"
#include "components/icons/rickyPageIcons.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/FileEditUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr size_t MAX_COMPONENT_BYTES = 255;
constexpr char MOVE_HERE_ENTRY[] = "\x01";

std::string cleanEntryName(std::string entry) {
  if (!entry.empty() && entry.back() == '/') entry.pop_back();
  return entry;
}

std::string joinPath(const std::string& parent, const std::string& name) {
  return parent == "/" ? "/" + name : parent + "/" + name;
}
}  // namespace

void formatFileName(const std::string& filename, char* buffer, size_t bufferSize);
void formatFileExtension(const std::string& filename, char* buffer, size_t bufferSize);
std::string getFileName(std::string filename);
std::string getFileExtension(const std::string& filename);

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, const Mode mode, const bool scoped)
    : UiListActivity("FileBrowser", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      mode(mode),
      basepath(initialPath.empty() ? "/" : std::move(initialPath)) {
  if (scoped) rootPath = basepath;
}

void FileBrowserActivity::loadFiles() {
  resetMetadata();
  prewarmedStart = -1;
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    rebuildRowItems();  // files is empty; also drops any now-stale cached rows
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    rebuildRowItems();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDirectory = file.isDirectory();
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (isDirectory) {
      const std::string entryName(fileNameBuffer.get());
      const std::string entryPath = joinPath(basepath, entryName);
      if (browserState == BrowserState::ChoosingMoveDestination &&
          (FsHelpers::isProtectedPathComponent(entryName) ||
           FsHelpers::isSameOrDescendantPath(entryPath, moveSourcePath))) {
        continue;
      }
      files.emplace_back(entryName + "/");
    } else {
      if (browserState == BrowserState::ChoosingMoveDestination || mode == Mode::PickLibraryFolder) continue;
      std::string_view filename{fileNameBuffer.get()};
      switch (mode) {
        case Mode::Books:
          files.emplace_back(filename);
          break;
        case Mode::PickLibraryBook:
          if (FsHelpers::hasReflowableBookExtension(filename)) files.emplace_back(filename);
          break;
        case Mode::PickLibraryFolder:
          break;
        case Mode::PickFirmware:
          if (FsHelpers::checkFileExtension(filename, ".bin")) files.emplace_back(filename);
          break;
        case Mode::PickPng:
          if (FsHelpers::hasPngExtension(filename)) files.emplace_back(filename);
          break;
        case Mode::PickAvatar:
        case Mode::PickWallpaper:
          if (FsHelpers::hasPngExtension(filename) || FsHelpers::hasBmpExtension(filename) ||
              FsHelpers::hasJpgExtension(filename))
            files.emplace_back(filename);
          break;
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
  if (browserState == BrowserState::ChoosingMoveDestination || mode == Mode::PickLibraryFolder)
    files.insert(files.begin(), MOVE_HERE_ENTRY);
  rebuildRowItems();
}

// Derives rowNames/rowExtensions/rowItems from `files`. Called whenever
// `files` changes (end of loadFiles()) so buildScreen() can reuse the cached
// rows on every repaint instead of re-deriving a name/extension string (and a
// ListItem) per file each time it's called.
void FileBrowserActivity::rebuildRowItems() {
  rowsUseFileIcons = UITheme::getInstance().getTheme().showsFileIcons();
  if (!usesIconLayout()) {
    rowNames.clear();
    rowExtensions.clear();
    rowItems.clear();
    gridLabels.clear();
    return;
  }
  rowNames.resize(files.size());
  rowExtensions.resize(files.size());
  rowItems.clear();
  rowItems.reserve(files.size());
  gridLabels.clear();
  if (usesIconLayout()) {
    gridLabels.resize(files.size());
    const int labelWidth = std::max(1, renderer.getScreenWidth() / InxGridGeometry::columns - 16);
    for (size_t i = 0; i < files.size(); ++i) {
      gridLabels[i] = renderer.truncatedText(UI_10_FONT_ID, getFileName(files[i]).c_str(), labelWidth);
    }
  }
  for (size_t i = 0; i < files.size(); i++) {
    rowNames[i] = files[i] == MOVE_HERE_ENTRY ? tr(STR_MOVE_HERE) : getFileName(files[i]);
    rowExtensions[i] = files[i] == MOVE_HERE_ENTRY ? "" : getFileExtension(files[i]);
    fui::ListItem item;
    item.label = rowNames[i].c_str();
    if (!rowExtensions[i].empty()) item.value = rowExtensions[i].c_str();
    if (files[i] != MOVE_HERE_ENTRY) item.icon = listIconFor(UITheme::getFileIcon(files[i]));
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }

  // One SD pass for every CJK filename in the folder; repaints then hit the
  // resident tables instead of re-reading per-string. Getter form: no
  // concatenated copy (a bare-new string append aborts under heap pressure).
  // The last index covers the bottom path band: basepath (possibly a CJK
  // folder name) draws in the same small font, so it must live in the same
  // batch or it would evict the rows' glyphs when the heap gate disables
  // union merging. (prewarmFallbackText appends the truncation ellipsis.)
  struct PrewarmCtx {
    const std::vector<std::string>* names;
    const std::string* path;
  } prewarmCtx{&rowNames, &basepath};
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < c->names->size() ? (*c->names)[i].c_str() : c->path->c_str();
      },
      &prewarmCtx, static_cast<uint32_t>(rowNames.size()) + 1);
}

bool FileBrowserActivity::usesIconLayout() const {
  // The shelf grid belongs to Library. Storage/pickers use the list contract
  // even when the legacy shelf layout setting is Icons.
  if (UiHighDpiProfile::enabled) return false;
  return mode == Mode::Books && browserState == BrowserState::Browsing && UITheme::getInstance().hasMainTabs() &&
         InxGridGeometry::layoutFrom(SETTINGS.inxLibraryLayout) == InxItemLayout::Icons;
}

void FileBrowserActivity::drawIconGrid(UiScreen& screen, const fui::Rect rect) const {
  const int start = InxGridGeometry::pageStart(nav.selected, files.size());
  constexpr int iconSize = 72;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const bool showSelection = showMainTabContentSelection();
  for (int slot = 0; slot < InxGridGeometry::itemsPerPage && start + slot < listCount(); ++slot) {
    const int index = start + slot;
    Rect cell = InxGridGeometry::cellBounds(slot, rect.width, rect.height);
    cell.x += rect.x;
    cell.y += rect.y;
    const bool selected = showSelection && index == nav.selected;
    if (selected) renderer.fillRect(cell.x, cell.y, cell.width, cell.height, true);
    const UIIcon type = UITheme::getFileIcon(files[index]);
    const uint8_t* icon = type == UIIcon::Folder ? FolderLarge : (type == UIIcon::Image ? ImageLarge : BookLarge);
    const int iconX = cell.x + (cell.width - iconSize) / 2;
    const int iconY = cell.y + std::max(4, (cell.height - iconSize - lineHeight - 6) / 2);
    if (selected)
      renderer.drawIconInverted(icon, iconX, iconY, iconSize);
    else
      renderer.drawIcon(icon, iconX, iconY, iconSize);
    const char* label =
        index < static_cast<int>(gridLabels.size()) ? gridLabels[index].c_str() : rowNames[index].c_str();
    const int labelX = cell.x + (cell.width - renderer.getTextWidth(UI_10_FONT_ID, label)) / 2;
    renderer.drawText(UI_10_FONT_ID, labelX, iconY + iconSize + 6, label, !selected);
    screen.frame().hit(fui::Rect{static_cast<int16_t>(cell.x), static_cast<int16_t>(cell.y),
                                 static_cast<int16_t>(cell.width), static_cast<int16_t>(cell.height)},
                       ACTION_ROW, static_cast<int16_t>(index), fui::InputTouch | fui::InputLongPress);
  }
  GUI.drawSideScrollBar(renderer, Rect{rect.x, rect.y, rect.width, rect.height}, files.size(), start,
                        InxGridGeometry::itemsPerPage);
}

void FileBrowserActivity::onEnter() {
  UiListActivity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    rootPath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    // The first screen build pulls the viewport to it (ListNav follow-on-build).
    nav.selected = static_cast<int>(findEntry(fileName));
  } else {
    loadFiles();
  }
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  rowNames.clear();
  rowExtensions.clear();
  gridLabels.clear();
  rowItems.clear();
  fileNameBuffer.reset();
  moveSourcePath.clear();
  moveSourceEntry.clear();
  moveReturnPath.clear();
}

void FileBrowserActivity::resetMetadata() {
  metadataStart_ = -1;
  metadataSlot_ = -1;
  metadata_ = {};
  requestedMetadataStart_ = 0;
  metadataRepaint_ = false;
}

void FileBrowserActivity::loop() {
  UiListActivity::loop();  // Input is always handled before background I/O.
  if (UiHighDpiProfile::enabled && fileNameBuffer) updateMetadata();
}

void FileBrowserActivity::updateMetadata() {
  const int requested = requestedMetadataStart_.load();
  if (requested != metadataStart_) {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    metadataStart_ = requested;
    metadata_ = {};
    metadataSlot_ = -1;
    metadataRepaint_ = false;
  }
  if (metadataSlot_ < 0) {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    // File sizes only. A folder label must never enumerate its children just
    // to display an item count (16 visible folders could mean 16 huge walks).
    for (int slot = 0; slot < METADATA_WINDOW; ++slot) {
      const int index = metadataStart_ + slot;
      if (index >= static_cast<int>(files.size())) break;
      const auto& entry = files[index];
      if (metadata_[slot].done || entry == MOVE_HERE_ENTRY || entry.empty()) continue;
      const bool directory = entry.back() == '/';
      if (directory) {
        metadata_[slot].done = true;
        continue;
      }
      const int length =
          snprintf(metadataPath_, sizeof(metadataPath_), "%s%s%.*s", basepath.c_str(), basepath == "/" ? "" : "/",
                   static_cast<int>(entry.size() - (directory ? 1 : 0)), entry.c_str());
      if (length < 0 || length >= static_cast<int>(sizeof(metadataPath_))) {
        metadata_[slot].done = true;
        continue;
      }
      metadataSlot_ = slot;
      break;
    }
  }
  if (metadataSlot_ < 0) {
    if (metadataRepaint_) {
      metadataRepaint_ = false;
      requestUpdate();
    }
    return;
  }
  uint64_t value = 0;
  bool done = false;
  bool known = false;
  {
    auto entry = Storage.open(metadataPath_);
    if (!entry) {
      done = true;
    } else if (!entry.isDirectory()) {
      value = entry.fileSize64();
      known = done = true;
    } else {
      done = true;  // directory replaced the file after listing: no child walk
    }
  }
  if (done) {
    RenderLock lock(*this);
    metadata_[metadataSlot_] = {value, true, known};
    metadataSlot_ = -1;
    metadataRepaint_ = true;
  }
  if (metadataRepaint_ && millis() - metadataRepaintAt_ >= 300) {
    metadataRepaint_ = false;
    metadataRepaintAt_ = millis();
    requestUpdate();
  }
}

void FileBrowserActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  moveSelectionTo(MainTabs::contentEdgeIndex(edge, listCount()));
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

std::string FileBrowserActivity::selectedPath() const {
  if (files.empty() || nav.selected < 0 || nav.selected >= listCount()) return {};
  if (files[nav.selected] == MOVE_HERE_ENTRY) return basepath;
  return joinPath(basepath, cleanEntryName(files[nav.selected]));
}

void FileBrowserActivity::showNotice(const StrId message) {
  constexpr StrId options[] = {StrId::STR_OK_BUTTON};
  editPopup.show(message, options, 1, 0, [](int) {});
  requestUpdate();
}

void FileBrowserActivity::showEditMenu() {
  if (mode != Mode::Books || browserState != BrowserState::Browsing || files.empty() || nav.selected < 0 ||
      nav.selected >= listCount()) {
    return;
  }

  const std::string entry = cleanEntryName(files[nav.selected]);
  if (FsHelpers::isProtectedPathComponent(entry)) return;

  if (UITheme::getInstance().hasMainTabs()) {
    const bool book = FsHelpers::hasReflowableBookExtension(files[nav.selected]);
    const char* actions[] = {tr(STR_LIBRARY_ADD_BOOK), tr(STR_RENAME), tr(STR_MOVE), tr(STR_DELETE)};
    static constexpr EditAction order[] = {EditAction::AddToLibrary, EditAction::Rename, EditAction::Move,
                                           EditAction::Delete};
    editPopup.show(entry.c_str(), actions + (book ? 0 : 1), book ? 4 : 3, 0, [this, book](const int index) {
      if (index >= 0 && index < (book ? 4 : 3)) executeEditAction(order[index + (book ? 0 : 1)]);
    });
  } else {
    constexpr StrId actions[] = {StrId::STR_OPEN, StrId::STR_DELETE, StrId::STR_RENAME, StrId::STR_MOVE};
    static constexpr EditAction order[] = {EditAction::Open, EditAction::Delete, EditAction::Rename, EditAction::Move};
    editPopup.show(StrId::STR_FILENAME, actions, static_cast<int>(std::size(actions)), 0, [this](const int index) {
      if (index >= 0 && index < static_cast<int>(std::size(order))) executeEditAction(order[index]);
    });
  }
  requestUpdate();
}

void FileBrowserActivity::executeEditAction(const EditAction action) {
  switch (action) {
    case EditAction::AddToLibrary:
      startActivityForResultWith<LibraryImportActivity>(
          [this](const ActivityResult&) { requestUpdate(); },
          LibraryImportRequest{LibraryImportRequest::Action::AddBook, selectedPath()});
      return;
    case EditAction::Rename:
      promptRename();
      return;
    case EditAction::Move:
      beginMove();
      return;
    case EditAction::Delete:
      if (!files.empty() && nav.selected >= 0 && nav.selected < listCount()) {
        promptDelete(selectedPath(), files[nav.selected]);
      }
      return;
    case EditAction::Open:
      activateSelected();
      return;
  }
}

void FileBrowserActivity::promptRename() {
  if (files.empty() || nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string entry = files[nav.selected];
  const std::string oldPath = selectedPath();
  const bool isDirectory = entry.back() == '/';
  const std::string extension = isDirectory ? std::string() : getFileExtension(entry);
  const size_t maxLength = MAX_COMPONENT_BYTES > extension.size() ? MAX_COMPONENT_BYTES - extension.size() : 0;
  const std::string initialName = isDirectory ? cleanEntryName(entry) : getFileName(entry);
  const int returnIndex = nav.selected;
  const std::string returnPath = basepath;

  auto handler = [this, oldPath, extension, isDirectory, returnIndex, returnPath](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;

    const std::string& name = std::get<KeyboardResult>(result.data).text;
    if (!FsHelpers::isValidPathComponent(name) || FsHelpers::isProtectedPathComponent(name)) {
      showNotice(StrId::STR_INVALID_FILE_NAME);
      return;
    }

    const std::string newEntry = FileEditUtils::withPreservedExtension(name, extension);
    const std::string newPath = joinPath(returnPath, newEntry);
    if (newPath == oldPath) return;
    if (Storage.exists(newPath.c_str())) {
      showNotice(StrId::STR_TARGET_EXISTS);
      return;
    }
    if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
      LOG_ERR("FileBrowser", "Failed to rename %s to %s", oldPath.c_str(), newPath.c_str());
      showNotice(StrId::STR_FILE_OPERATION_FAILED);
      return;
    }

    const bool dataOk = relocatePathData(oldPath, newPath, isDirectory);
    finishEdit(returnPath, returnIndex, newEntry + (isDirectory ? "/" : ""));
    if (!dataOk) showNotice(StrId::STR_FILE_DATA_MIGRATION_FAILED);
  };

  if (!startActivityForResultWith<KeyboardEntryActivity>(std::move(handler), tr(STR_RENAME), initialName, maxLength,
                                                         InputType::Text)) {
    showNotice(StrId::STR_FILE_OPERATION_FAILED);
  }
}

void FileBrowserActivity::beginMove() {
  if (files.empty() || nav.selected < 0 || nav.selected >= listCount()) return;
  moveSourceEntry = files[nav.selected];
  moveSourcePath = selectedPath();
  moveReturnPath = basepath;
  moveReturnIndex = nav.selected;
  moveSourceIsDirectory = moveSourceEntry.back() == '/';
  browserState = BrowserState::ChoosingMoveDestination;
  {
    RenderLock lock(*this);
    loadFiles();
    nav.selected = 0;
    nav.top = 0;
  }
  requestUpdate();
}

void FileBrowserActivity::cancelMove() {
  const std::string returnPath = moveReturnPath;
  const std::string sourceEntry = moveSourceEntry;
  const int returnIndex = moveReturnIndex;
  browserState = BrowserState::Browsing;
  finishEdit(returnPath, returnIndex, sourceEntry);
  moveSourcePath.clear();
  moveSourceEntry.clear();
  moveReturnPath.clear();
}

void FileBrowserActivity::completeMove() {
  if (browserState != BrowserState::ChoosingMoveDestination || moveSourcePath.empty()) return;

  const std::string cleanSourceEntry = cleanEntryName(moveSourceEntry);
  const std::string newPath = joinPath(basepath, cleanSourceEntry);
  const auto destinationError = FileEditUtils::validateMoveDestination(
      moveSourcePath, moveReturnPath, basepath, moveSourceIsDirectory, Storage.exists(newPath.c_str()));
  switch (destinationError) {
    case FileEditUtils::MoveDestinationError::None:
      break;
    case FileEditUtils::MoveDestinationError::SameDirectory:
    case FileEditUtils::MoveDestinationError::OwnDescendant:
      showNotice(StrId::STR_CANNOT_MOVE_HERE);
      return;
    case FileEditUtils::MoveDestinationError::TargetExists:
      showNotice(StrId::STR_TARGET_EXISTS);
      return;
  }
  if (!Storage.rename(moveSourcePath.c_str(), newPath.c_str())) {
    LOG_ERR("FileBrowser", "Failed to move %s to %s", moveSourcePath.c_str(), newPath.c_str());
    showNotice(StrId::STR_FILE_OPERATION_FAILED);
    return;
  }

  const bool dataOk = relocatePathData(moveSourcePath, newPath, moveSourceIsDirectory);
  const std::string returnPath = moveReturnPath;
  const int returnIndex = moveReturnIndex;
  browserState = BrowserState::Browsing;
  finishEdit(returnPath, returnIndex);
  moveSourcePath.clear();
  moveSourceEntry.clear();
  moveReturnPath.clear();
  if (!dataOk) showNotice(StrId::STR_FILE_DATA_MIGRATION_FAILED);
}

bool FileBrowserActivity::relocateDirectoryData(const std::string& oldPath, const std::string& newPath) {
  if (!fileNameBuffer) return false;

  bool ok = true;
  std::vector<std::string> directories;
  directories.reserve(16);
  directories.push_back(newPath);
  while (!directories.empty()) {
    std::string directoryPath = std::move(directories.back());
    directories.pop_back();
    auto directory = Storage.open(directoryPath.c_str());
    if (!directory || !directory.isDirectory()) {
      LOG_ERR("FileBrowser", "Failed to scan moved directory: %s", directoryPath.c_str());
      ok = false;
      continue;
    }

    directory.rewindDirectory();
    for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) continue;
      const std::string entryPath = joinPath(directoryPath, fileNameBuffer.get());
      if (entry.isDirectory()) {
        directories.push_back(entryPath);
        continue;
      }
      const std::string oldEntryPath = FsHelpers::rebasePath(entryPath, newPath, oldPath);
      if (!relocateBookArtifacts(oldEntryPath, entryPath)) ok = false;
    }
  }

  if (!RECENT_BOOKS.updatePathPrefix(oldPath, newPath)) ok = false;
  if (!READING_STATS.updateBookPathPrefix(oldPath, newPath)) ok = false;
  if (FsHelpers::isSameOrDescendantPath(APP_STATE.openEpubPath, oldPath)) {
    APP_STATE.openEpubPath = FsHelpers::rebasePath(APP_STATE.openEpubPath, oldPath, newPath);
    if (!APP_STATE.saveToFile()) ok = false;
  }
  return ok;
}

bool FileBrowserActivity::relocatePathData(const std::string& oldPath, const std::string& newPath,
                                           const bool isDirectory) {
  if (isDirectory) return relocateDirectoryData(oldPath, newPath);
  return relocateBookArtifacts(oldPath, newPath) && relocateBookReferences(oldPath, newPath);
}

void FileBrowserActivity::finishEdit(const std::string& returnPath, const int fallbackIndex,
                                     const std::string& selectedEntry) {
  RenderLock lock(*this);
  RickyStorageActivity::invalidateScan();  // files may have arrived or left
  basepath = returnPath;
  loadFiles();
  if (!selectedEntry.empty()) {
    nav.selected = static_cast<int>(findEntry(selectedEntry));
  } else if (files.empty()) {
    nav.selected = 0;
  } else {
    nav.selected = std::clamp(fallbackIndex, 0, listCount() - 1);
  }
  nav.follow(listCount());
  lock.unlock();
  requestUpdate(true);
}

void FileBrowserActivity::promptDelete(const std::string& fullPath, const std::string& entry) {
  const int returnIndex = nav.selected;
  auto handler = [this, fullPath, returnIndex](const ActivityResult& result) {
    if (result.isCancelled) return;
    if (removeDirFile(fullPath)) {
      finishEdit(basepath, returnIndex);
      return;
    }
    LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
    showNotice(StrId::STR_FILE_OPERATION_FAILED);
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  if (!startActivityForResultWith<ConfirmationActivity>(std::move(handler), heading, cleanEntryName(entry))) {
    showNotice(StrId::STR_FILE_OPERATION_FAILED);
  }
}

void FileBrowserActivity::activateIndex(const int index) {
  (void)index;  // base already synced nav.selected to the tapped row
  // Activation navigates or opens; a lingering flash would gray an unrelated
  // row on the next list.
  app.clearTapFlash();
  activateSelected();
}

void FileBrowserActivity::onRowLongPress(const int index) {
  (void)index;  // base already synced nav.selected to the pressed row
  app.clearTapFlash();
  if (mode != Mode::Books) {
    activateSelected();
  } else {
    showEditMenu();
  }
}

void FileBrowserActivity::activateSelected() {
  if (files.empty()) return;
  // A touch activation can carry a row index captured before a delete/reload
  // shrank the list; the next render re-registers the rows.
  if (nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string& entry = files[nav.selected];
  if (mode == Mode::PickLibraryFolder && entry == MOVE_HERE_ENTRY) {
    setResult(ActivityResult{FilePathResult{basepath}});
    finish();
    return;
  }
  if (browserState == BrowserState::ChoosingMoveDestination && entry == MOVE_HERE_ENTRY) {
    completeMove();
    return;
  }
  const bool isDirectory = (entry.back() == '/');

  if (mode != Mode::Books && !isDirectory) {
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (UITheme::getInstance().hasMainTabs() && mode == Mode::Books && browserState == BrowserState::Browsing &&
      mappedInput.getHeldTime() >= GO_HOME_MS) {
    showEditMenu();
    return;
  }

  RenderLock lock(*this);
  if (basepath.back() != '/') basepath += "/";
  if (isDirectory) {
    basepath += entry.substr(0, entry.length() - 1);
    loadFiles();
    nav.selected = 0;
    nav.top = 0;
    lock.unlock();
    requestUpdate();
  } else {
    const std::string fullPath = basepath + entry;
    lock.unlock();
    if (!FsHelpers::hasReflowableBookExtension(fullPath) && !FsHelpers::hasBmpExtension(fullPath) &&
        !FsHelpers::hasPngExtension(fullPath) && !FsHelpers::hasJpgExtension(fullPath)) {
      showNotice(StrId::STR_RICKY_NO_PREVIEW);
      return;
    }
    onSelectBook(fullPath);
  }
}

bool FileBrowserActivity::handleCustomInput() {
  if (editPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (usesIconLayout()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int next =
          swipe == MappedInputManager::SwipeDir::Up
              ? ButtonNavigator::nextPageIndex(nav.selected, listCount(), InxGridGeometry::itemsPerPage)
              : ButtonNavigator::previousPageIndex(nav.selected, listCount(), InxGridGeometry::itemsPerPage);
      moveSelectionTo(next);
      return true;
    }
  }

  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && browserState == BrowserState::Browsing &&
      mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != rootPath) {
    {
      // buildScreen() runs on the render task and reads basepath plus the
      // row caches rebuildRowItems() frees; mutate only under the render lock.
      RenderLock lock(*this);
      basepath = rootPath;
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
    }
    requestUpdate();
    return true;
  }

  return false;
}

bool FileBrowserActivity::handleButtons() {
  if (!UITheme::getInstance().hasMainTabs() && mode == Mode::Books && browserState == BrowserState::Browsing &&
      mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, GO_HOME_MS)) {
    app.clearTapFlash();
    showEditMenu();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != rootPath) {
        const std::string oldPath = basepath;

        {
          // buildScreen() runs on the render task and reads basepath plus the
          // row caches rebuildRowItems() frees; mutate only under the render lock.
          RenderLock lock(*this);
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          nav.selected = static_cast<int>(findEntry(dirName));
          nav.top = 0;
          nav.follow(listCount());
        }

        requestUpdate();
      } else if (browserState == BrowserState::ChoosingMoveDestination) {
        cancelMove();
      } else if (mode != Mode::Books) {
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        // RickyOS file browsing is a child of Storage. Finish the child instead
        // of replacing the entire activity stack with Home.
        finish();
      }
    }
    return true;
  }

  return false;
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Full path band: left-truncated so the
  // deepest directory stays visible.
  {
    const int pathFont = UiHighDpiProfile::enabled
                             ? RickyDesignUi::fontFor(renderer, RickyDesignUi::CAPTION, basepath.c_str())
                             : SMALL_FONT_ID;
    const int pathLineHeight = renderer.getLineHeight(pathFont);
    const fui::Rect band = UiHighDpiProfile::enabled
                               ? screen.takeTop(ricky::ds::layout::kSectionHeaderHeight)
                               : screen.takeBottom(static_cast<int16_t>(pathLineHeight + metrics.verticalSpacing));
    if (!UiHighDpiProfile::enabled)
      screen.target().fill(fui::Rect{band.x, band.y, band.width, 3}, fui::Paint::solid(fui::Color::Black));
    const int pathY =
        band.y + metrics.verticalSpacing / 2 + (band.height - metrics.verticalSpacing / 2 - pathLineHeight) / 2;
    const int pathMaxWidth = band.width - metrics.contentSidePadding * 2;
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(pathFont, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(pathFont, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(pathFont, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(pathFont, band.x + metrics.contentSidePadding, pathY, pathDisplay);
  }

  if (files.empty()) {
    screen.centeredText(mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND),
                        screen.theme().bodyText);
    return;
  }

  // rowNames/rowExtensions/rowItems are built once per loadFiles() call (see
  // rebuildRowItems()) and reused here. getFileName()'s folder-bracket format
  // depends on the theme, so a theme change picked up while this activity was
  // paused underneath another screen invalidates the cache before it's read.
  if (rowsUseFileIcons != UITheme::getInstance().getTheme().showsFileIcons()) {
    rebuildRowItems();
  }

  if (usesIconLayout()) {
    nav.visibleRows = InxGridGeometry::itemsPerPage;
    nav.drawnRows = InxGridGeometry::itemsPerPage;
    nav.top = InxGridGeometry::pageStart(nav.selected, listCount());
    drawIconGrid(screen, screen.body());
    return;
  }

  fui::ListProps props;
  props.rowProvider = &FileBrowserActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(files.size());
  props.action = ACTION_ROW;
  // Tap opens/navigates; long-press prompts delete (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the extension and the row edge
  // Match the pre-FreeInkUI file-list size while keeping two-line wrapping for
  // long names.
  fui::TextStyle label = UITheme::getInstance().hasMainTabs() ? screen.theme().bodyText : screen.theme().smallText;
  label.maxLines = 2;
  props.labelText = label;
  if (UiHighDpiProfile::enabled) {
    props.rowHeight = ricky::ds::layout::kRowHeightTwoLine;
    props.rowInset = screen.theme().listInset;
    props.sidePadding = screen.theme().listSidePadding;
    props.subtitleText = screen.theme().smallText;
    props.subtitleText.color = fui::Color::DarkGray;
    props.subtitleText.maxLines = 1;
    props.iconSize = 32;
    props.textGap = ricky::ds::space::kInlineGap;
    props.labelText.maxLines = 2;
  }
  // The trailing value here is just the short extension: skip the balanced
  // 60%-band wrap cap and let both name lines run the full width before it.
  props.balanceWrappedLabelWithValue = false;
  // Wrapped two-line names shrink how many rows fit a page, so the last row
  // of a page can end up in leftover space: draw it as a partial preview so
  // files past the fold are visibly present, not silently absent.
  props.partialTrailingRow = UITheme::getInstance().hasMainTabs();
  syncListViewport(screen, props);
  requestedMetadataStart_.store(nav.top);
  rowLabelWidth_ = std::max(1, screen.body().width - std::max<int>(0, props.rowInset) * 2 -
                                   std::max<int>(0, props.sidePadding) * 2 - props.iconSize - props.textGap);
  prewarmRowGlyphs(nav.top);
  screen.list(props);
}

void FileBrowserActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      browserState == BrowserState::ChoosingMoveDestination ? std::string(tr(STR_MOVE))
      : mode == Mode::PickFirmware                          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
      : mode == Mode::PickLibraryBook                       ? std::string(tr(STR_LIBRARY_ADD_BOOK))
      : mode == Mode::PickLibraryFolder                     ? std::string(tr(STR_LIBRARY_ADD_FOLDER))
      : mode == Mode::PickPng                               ? std::string(tr(STR_CUSTOM_IMAGE))
      : mode == Mode::PickWallpaper
          ? std::string(tr(STR_RICKY_SELECT_WALLPAPER))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  drawPageHeader(Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());
}

void FileBrowserActivity::drawFooter() {
  const char* backLabel =
      browserState == BrowserState::ChoosingMoveDestination
          ? tr(STR_BACK)
          : ((basepath == rootPath) ? (mode != Mode::Books ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK));
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFile = mode != Mode::Books && !files.empty() && nav.selected >= 0 && nav.selected < listCount() &&
                             files[nav.selected].back() != '/';
  const bool movingHere = browserState == BrowserState::ChoosingMoveDestination && !files.empty() &&
                          nav.selected >= 0 && nav.selected < listCount() && files[nav.selected] == MOVE_HERE_ENTRY;
  const char* confirmLabel =
      files.empty() ? "" : (movingHere ? tr(STR_MOVE_HERE) : (selectingFile ? tr(STR_SELECT) : tr(STR_OPEN)));
  const auto labels = mainTabButtonLabels(backLabel, confirmLabel, !files.empty());
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FileBrowserActivity::render(RenderLock&& lock) {
  if (editPopup.isActive()) renderPopupUnderlay();
  if (editPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  const auto found = std::find(files.begin(), files.end(), name);
  return found == files.end() ? 0 : static_cast<size_t>(found - files.begin());
}

void formatFileName(const std::string& filename, char* buffer, const size_t bufferSize) {
  if (filename.empty()) {
    buffer[0] = '\0';
    return;
  }
  const bool isDirectory = filename.back() == '/';
  const size_t dot = isDirectory ? filename.size() - 1 : filename.rfind('.');
  const int length = static_cast<int>(dot == std::string::npos ? filename.size() : dot);
  const char* format = isDirectory && !UITheme::getInstance().getTheme().showsFileIcons() ? "[%.*s]" : "%.*s";
  snprintf(buffer, bufferSize, format, length, filename.c_str());
  // Compose only the display copy; filesystem lookup needs the raw entry bytes.
  utf8ComposeNfcInPlace(buffer);
}

void formatFileExtension(const std::string& filename, char* buffer, const size_t bufferSize) {
  buffer[0] = '\0';
  if (filename.empty() || filename.back() == '/') return;
  if (const char* extension = strrchr(filename.c_str(), '.')) snprintf(buffer, bufferSize, "%s", extension);
}

void FileBrowserActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<FileBrowserActivity*>(ctx);
  if (index >= self->files.size()) return;
  const std::string& entry = self->files[index];
  if (entry == MOVE_HERE_ENTRY) {
    item.label = self->mode == Mode::PickLibraryFolder ? tr(STR_LIBRARY_SELECT_FOLDER) : tr(STR_MOVE_HERE);
    item.actionValue = static_cast<int16_t>(index);
    return;
  }
  formatFileName(entry, self->rowNameBuf, sizeof(self->rowNameBuf));
  item.label = self->rowNameBuf;
  if (UiHighDpiProfile::enabled) {
    // Filesystem names keep their suffix; folder names omit the legacy []
    // notation. Formatting touches only the visible provider scratch row.
    const bool directory = !entry.empty() && entry.back() == '/';
    snprintf(self->rowNameBuf, sizeof(self->rowNameBuf), "%.*s", static_cast<int>(entry.size() - (directory ? 1 : 0)),
             entry.c_str());
    utf8ComposeNfcInPlace(self->rowNameBuf);
    const int font = RickyDesignUi::fontFor(self->renderer, RickyDesignUi::BODY, self->rowNameBuf);
    RickyFileLabel::middleEllipsis(self->rowNameBuf, self->rowLabelWidth_ * 2 - self->renderer.getLineHeight(font),
                                   [&](const char* text) { return self->renderer.getTextWidth(font, text); });
    formatFileExtension(entry, self->rowExtBuf, sizeof(self->rowExtBuf));
    if (directory) {
      item.subtitle = tr(STR_RICKY_FOLDER_TYPE);
      item.icon = fui::bitmapFromIcon(icon_ricky_row_folder_32);
    } else {
      char* extension = self->rowExtBuf;
      if (*extension == '.') ++extension;
      for (char* p = extension; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
      }
      item.subtitle = extension;
      const auto kind = UITheme::getFileIcon(entry);
      item.icon = fui::bitmapFromIcon(kind == UIIcon::Book    ? icon_ricky_row_books_32
                                      : kind == UIIcon::Text  ? icon_ricky_row_text_32
                                      : kind == UIIcon::Image ? icon_ricky_row_image_32
                                                              : icon_ricky_row_file_32);
    }
    const int slot = static_cast<int>(index) - self->metadataStart_;
    if (slot >= 0 && slot < METADATA_WINDOW && self->metadata_[slot].known) {
      const auto value = self->metadata_[slot].value;
      if (directory) {
        snprintf(self->rowInfoBuf, sizeof(self->rowInfoBuf), tr(STR_RICKY_FOLDER_COUNT), static_cast<unsigned>(value));
      } else {
        // Bytes from the file's actual directory entry, never inferred from its format.
        const double size = static_cast<double>(value);
        const char* unit = value >= 1024ULL * 1024 * 1024 ? "GB"
                           : value >= 1024 * 1024         ? "MB"
                           : value >= 1024                ? "KB"
                                                          : "B";
        const double divisor = unit[0] == 'G'   ? 1073741824.0
                               : unit[0] == 'M' ? 1048576.0
                               : unit[0] == 'K' ? 1024.0
                                                : 1.0;
        snprintf(self->rowInfoBuf, sizeof(self->rowInfoBuf), "%s · %.*f %s", item.subtitle,
                 value >= 1024 * 1024 ? 1 : 0, size / divisor, unit);
      }
      item.subtitle = self->rowInfoBuf;
    }
    item.actionValue = static_cast<int16_t>(index);
    return;
  }
  formatFileExtension(entry, self->rowExtBuf, sizeof(self->rowExtBuf));
  if (self->rowExtBuf[0] != '\0') {
    item.value = self->rowExtBuf;
  }
  item.icon = listIconFor(UITheme::getFileIcon(entry));
  item.actionValue = static_cast<int16_t>(index);
}

void FileBrowserActivity::prewarmRowGlyphs(const int start) {
  const int total = static_cast<int>(files.size());
  int clamped = start;
  if (clamped > total - PREWARM_WINDOW) clamped = total - PREWARM_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == prewarmedStart) return;
  prewarmedStart = clamped;
  const int count = total - clamped < PREWARM_WINDOW ? total - clamped : PREWARM_WINDOW;

  struct PrewarmCtx {
    FileBrowserActivity* self;
    int first;
    int count;
  } prewarmCtx{this, clamped, count};
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        auto* c = const_cast<PrewarmCtx*>(static_cast<const PrewarmCtx*>(ctx));
        if (i < static_cast<uint32_t>(c->count)) {
          formatFileName(c->self->files[c->first + i], c->self->rowNameBuf, sizeof(c->self->rowNameBuf));
          return c->self->rowNameBuf;
        }
        return c->self->basepath.c_str();
      },
      &prewarmCtx, static_cast<uint32_t>(count) + 1);
}
