#include "LibrarySourcesActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RickyStorageActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
namespace {
template <typename Screen>
void contentMargins(Screen& screen, const Rect& bounds, int height) {
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0, static_cast<int16_t>(height - bounds.y - bounds.height), 0});
}
}  // namespace

const char* LibraryImportActivity::headerTitle() const { return tr(STR_LIBRARY_IMPORT_TITLE); }
void LibraryImportActivity::onEnter() {
  UiListActivity::onEnter();
  waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                   mappedInput.isPressed(MappedInputManager::Button::Back);
  nav.selected = 3;
  // Only the explicit import owns a worker and its 8 KB stack. The shelf and
  // Storage allocate no scanning task when entered.
  if (xTaskCreate(&worker, "LibraryImport", 8192, this, 1, nullptr) != 1) {
    LOG_ERR("LIBSRC", "cannot allocate import task");
    done.store(true);
  }
}
void LibraryImportActivity::onExit() {
  cancel.store(true);
  while (!done.load()) delay(1);
  UiListActivity::onExit();
}
void LibraryImportActivity::worker(void* ctx) {
  auto* self = static_cast<LibraryImportActivity*>(ctx);
  self->succeeded = self->runImport();
  self->done.store(true);  // publishes stats/succeeded, no self access after this
  vTaskDelete(nullptr);
}
bool LibraryImportActivity::runImport() {
  auto sources = makeUniqueNoThrow<library::LibrarySources>();
  if (!sources) {
    LOG_ERR("LIBSRC", "source plan allocation failed");
    return false;
  }
  const auto loaded = sources->load();
  if (loaded == library::LibrarySources::Load::Error || cancel.load()) return false;
  const auto action = request.action;
  using Action = LibraryImportRequest::Action;
  if (action == Action::AddBook || action == Action::AddFolder) {
    HalFile target = Storage.open(request.path.c_str());
    if (!target || target.isDirectory() != (action == Action::AddFolder)) return false;
    const auto kind = action == Action::AddBook ? library::SourceKind::Book
                      : request.recursive       ? library::SourceKind::RecursiveFolder
                                                : library::SourceKind::Folder;
    if (!sources->add(request.path.c_str(), kind)) return false;
  } else if (action == Action::ExcludeBook) {
    if (!sources->exclude(request.path.c_str())) return false;
  } else if (action == Action::RemoveSource) {
    if (request.source >= sources->size()) return false;
    sources->erase(request.source);
  } else if (action == Action::RefreshSource) {
    if (request.source >= sources->size()) return false;
  }
  // The shelf before managed sources is not carried over: the Library starts empty
  // and holds exactly what the reader enrolls.
  if (!library::LibrarySources::retireLegacy()) return false;
  if (cancel.load() || !sources->save(true)) return false;
  library::markLibraryIndexDirty();
  library::BuildOptions build;
  build.sources = sources.get();
  // Every merge keeps only books an enrolled source still covers, so nothing from
  // an earlier whole-card index survives into the managed shelf.
  if (action == Action::ExcludeBook || action == Action::RemoveSource) {
    build.mergeExisting = true;
    build.filterExistingSources = true;
  }
  if (action == Action::AddBook || action == Action::AddFolder) {
    build.mergeExisting = true;
    build.filterExistingSources = true;
    for (uint16_t i = 0; i < sources->size(); ++i)
      if (request.path == (*sources)[i].path) {
        build.sourceToScan = i;
        break;
      }
  }
  if (action == Action::RefreshSource) {
    // One source re-read from the card; every other book replays from the index.
    build.mergeExisting = true;
    build.filterExistingSources = true;
    build.sourceToScan = request.source;
  }
  build.context = this;
  build.shouldCancel = [](void* ctx) { return static_cast<LibraryImportActivity*>(ctx)->cancel.load(); };
  build.progress = [](uint16_t count, void* ctx) { static_cast<LibraryImportActivity*>(ctx)->progress.store(count); };
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0, &build);
  if (!library::LibrarySources::finishUpdate(ok, loaded == library::LibrarySources::Load::Ready))
    LOG_ERR("LIBSRC", "source manifest %s deferred", ok ? "backup cleanup" : "rollback");
  LOG_INF("LIBSRC", "import %s books=%u walk=%lums cancelled=%d", ok ? "complete" : "failed", stats.books,
          static_cast<unsigned long>(stats.walkMs), stats.cancelled);
  return ok;
}
void LibraryImportActivity::loop() {
  if (done.load() && succeeded) {
    RickyStorageActivity::invalidateScan();
    setResult(ActivityResult{IntervalResult{stats.books}});
    finish();
    return;
  }
  if ((done.load() && seen != UINT16_MAX) || (!done.load() && progress.load() != seen && millis() - lastPaint > 800)) {
    seen = progress.load();
    if (done.load()) seen = UINT16_MAX;
    lastPaint = millis();
    requestUpdate();
  }
  UiListActivity::loop();
}
void LibraryImportActivity::onBackButton() {
  if (!done.load()) {
    cancel.store(true);
    requestUpdate();
    return;
  }
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
void LibraryImportActivity::activateIndex(int index) {
  if (index == 3) onBackButton();
}
bool LibraryImportActivity::handleCustomInput() {
  if (!waitForButtons) return false;
  waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                   mappedInput.isPressed(MappedInputManager::Button::Back);
  return true;
}
void LibraryImportActivity::buildScreen(UiScreen& screen) {
  contentMargins(screen, pageContentRect(), renderer.getScreenHeight());
  snprintf(progressText, sizeof(progressText), tr(STR_LIBRARY_IMPORT_COUNT), static_cast<unsigned>(progress.load()));
  fui::ListItem items[4];
  items[0].label = done.load() ? (cancel.load() ? tr(STR_LIBRARY_IMPORT_CANCELLED) : tr(STR_LIBRARY_REBUILD_FAILED))
                               : (cancel.load() ? tr(STR_LIBRARY_IMPORT_CANCELLING) : tr(STR_LIBRARY_REBUILDING));
  items[1].label = request.path.empty() ? tr(STR_LIBRARY_SOURCES) : request.path.c_str();
  items[2].label = progressText;
  items[3].label = done.load() ? tr(STR_DONE) : tr(STR_CANCEL);
  for (int i = 0; i < 4; ++i) items[i].actionValue = i;
  fui::ListProps props;
  props.items = items;
  props.count = 4;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

const char* LibrarySourcesActivity::headerTitle() const { return tr(STR_LIBRARY_SOURCES); }
void LibrarySourcesActivity::reload() {
  RenderLock lock(*this);
  sources = makeUniqueNoThrow<library::LibrarySources>();
  loadFailed = !sources || sources->load() == library::LibrarySources::Load::Error;
  if (loadFailed) sources.reset();
  nav.reset();
  requestUpdate();
}
void LibrarySourcesActivity::onEnter() {
  UiListActivity::onEnter();
  waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                   mappedInput.isPressed(MappedInputManager::Button::Back);
  reload();
}
void LibrarySourcesActivity::onExit() {
  sources.reset();
  UiListActivity::onExit();
}
bool LibrarySourcesActivity::handleCustomInput() {
  if (waitForButtons) {
    waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                     mappedInput.isPressed(MappedInputManager::Button::Back);
    return true;
  }
  return options.handleInput(mappedInput, [this] { requestUpdate(); });
}
void LibrarySourcesActivity::render(RenderLock&& lock) {
  if (options.isActive()) renderPopupUnderlay();
  if (options.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
void LibrarySourcesActivity::provideRow(void* ctx, uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<LibrarySourcesActivity*>(ctx);
  if (!self.sources || index >= self.listCount()) return;
  item.actionValue = index;
  if (index < 3) {
    static constexpr StrId labels[] = {StrId::STR_LIBRARY_ADD_BOOK, StrId::STR_LIBRARY_ADD_FOLDER,
                                       StrId::STR_LIBRARY_REBUILD};
    item.label = I18N.get(labels[index]);
    return;
  }
  const auto& source = (*self.sources)[index - 3];
  item.label = source.path;
  using Kind = library::SourceKind;
  item.value = source.kind == Kind::Book              ? tr(STR_LIBRARY_SOURCE_BOOK)
               : source.kind == Kind::ExcludedBook    ? tr(STR_LIBRARY_SOURCE_EXCLUDED)
               : source.kind == Kind::RecursiveFolder ? tr(STR_LIBRARY_SOURCE_RECURSIVE)
                                                      : tr(STR_LIBRARY_SOURCE_FOLDER);
}
void LibrarySourcesActivity::buildScreen(UiScreen& screen) {
  contentMargins(screen, pageContentRect(), renderer.getScreenHeight());
  if (loadFailed) {
    screen.centeredText(tr(STR_LIBRARY_SOURCES_FAILED));
    return;
  }
  fui::ListProps props;
  props.rowProvider = &provideRow;
  props.rowProviderCtx = this;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
void LibrarySourcesActivity::import(LibraryImportRequest request) {
  // Parent releases its rules before the child allocates a plan (normally 2 KB).
  {
    RenderLock lock(*this);
    sources.reset();
  }
  if (!startActivityForResultWith<LibraryImportActivity>([this](const ActivityResult&) { reload(); },
                                                         std::move(request)))
    reload();
}
void LibrarySourcesActivity::pick(bool folder) {
  const auto mode = folder ? FileBrowserActivity::Mode::PickLibraryFolder : FileBrowserActivity::Mode::PickLibraryBook;
  startActivityForResultWith<FileBrowserActivity>(
      [this, folder](const ActivityResult& result) {
        if (result.isCancelled || !std::holds_alternative<FilePathResult>(result.data)) return;
        const auto path = std::get<FilePathResult>(result.data).path;
        if (!folder) {
          import({LibraryImportRequest::Action::AddBook, path});
          return;
        }
        static constexpr StrId choices[] = {StrId::STR_LIBRARY_SOURCE_FOLDER, StrId::STR_LIBRARY_SOURCE_RECURSIVE};
        options.show(StrId::STR_LIBRARY_ADD_FOLDER, choices, 2, 0, [this, path](int choice) {
          if (choice < 0 || choice > 1) return;
          import({LibraryImportRequest::Action::AddFolder, path, choice == 1});
        });
        requestUpdate();
      },
      "/", mode);  // the card root: no folder is assumed to hold the books
}
void LibrarySourcesActivity::activateIndex(int index) {
  if (!sources || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (index < 2) {
    pick(index == 1);
    return;
  }
  if (index == 2) {
    import({});
    return;
  }
  const uint16_t source = index - 3;
  const auto kind = (*sources)[source].kind;
  const bool exclusion = kind == library::SourceKind::ExcludedBook;
  const bool folder = kind == library::SourceKind::Folder || kind == library::SourceKind::RecursiveFolder;
  const std::string path = (*sources)[source].path;
  // A folder opens first: it is how the Library's books are found on the card.
  const char* labels[] = {tr(STR_LIBRARY_OPEN_FOLDER), tr(STR_LIBRARY_REBUILD),
                          exclusion ? tr(STR_LIBRARY_RESTORE_BOOK) : tr(STR_LIBRARY_REMOVE_SOURCE)};
  const int offset = folder ? 0 : 1;
  options.show(tr(STR_LIBRARY_SOURCES), path.c_str(), labels + offset, 3 - offset, 0,
               [this, source, exclusion, path, offset](int picked) {
                 const int choice = picked + offset;
                 if (choice == 0) {
                   // Back climbs to the folder itself, then returns to this list.
                   startActivityForResultWith<FileBrowserActivity>([this](const ActivityResult&) { reload(); }, path,
                                                                   FileBrowserActivity::Mode::Books, /*scoped=*/true);
                 } else if (choice == 1) {
                   import({LibraryImportRequest::Action::RefreshSource, "", false, source});
                 } else if (choice == 2) {
                   if (exclusion)
                     import({LibraryImportRequest::Action::AddBook, path});
                   else
                     import({LibraryImportRequest::Action::RemoveSource, "", false, source});
                 }
               });
  requestUpdate();
}
