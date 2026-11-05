#include "EpubReaderActivity.h"

#include "util/ReadingBackground.h"

#include <RickyPerf.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/css/CssParser.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontCache.h>
#include <TrustedTime.h>
#include <Txt.h>
#include <VectorFontSupport.h>
#include <WeReadStore.h>
#include <WiFi.h>
#include <esp_system.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <PlusReaderImageLayout.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <utility>
#if CROSSPOINT_VECTOR_FONTS
#include <TtfEpdFont.h>
#endif

#include "AchievementsStore.h"
#include "BleInput.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnoteSelectActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderChrome.h"
#include "ReaderFontSizes.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/apps/weread/WeReadProgressSyncActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/FontPreloadView.h"
#include "components/RickyBadgeNotice.h"
#include "components/RickyCompanion.h"
#include "components/UITheme.h"
#include "components/icons/rickyReaderDog.h"
#include "fontIds.h"
#include "util/AchievementPopupUtils.h"
#include "util/BookCacheUtils.h"
#include "util/BookmarkFile.h"
#include "util/ReadingExcerpt.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ReadingGuideLine.h"
#include "util/ScreenshotUtil.h"
#include "util/TimeUtils.h"

namespace {
constexpr uint8_t MAX_PAGE_TURN_RATE = 30;
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
// EEGO A4 runs the same grayscale AA pass on its UC8279C 4-gray panel, so its
// overlay paints need the identical full-waveform treatment: a FAST differential
// after the AA waveform leaves the covered page ghosting through the chrome.
bool xteinkClassPanel() {
  return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_EEGO_A4;
}

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

// Body-text weight (RickyOS 字重): a curve over the 0..15 glyph coverage. Index 2 is the
// font as drawn; the others are gamma 2.0 / 1.4 (lighter edges, thinner strokes), 0.45
// (darker edges) and 0.65 on a one-pixel spread (readerTextWeightSpread). Layout never changes.
uint8_t readerTextWeight() { return SETTINGS.rickyTextWeight; }

const uint8_t* readerTextWeightCurve(const uint8_t weight) {
  static constexpr uint8_t kCurves[5][16] = {
      {0, 0, 0, 1, 1, 2, 2, 3, 4, 5, 7, 8, 10, 11, 13, 15},
      {0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15},
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
      {0, 4, 6, 7, 8, 9, 10, 11, 11, 12, 12, 13, 14, 14, 15, 15},
      {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 12, 13, 14, 14, 15},
  };
#if FREEINK_DEVICE_METALIO_PLUS
  // Image pages remain native gray with text AA off. Quantize only glyph
  // coverage in that case, preserving the user's black/white text choice.
  static constexpr auto kBinaryCurves = [] {
    std::array<std::array<uint8_t, 16>, 5> result{};
    for (size_t w = 0; w < result.size(); ++w)
      for (size_t i = 0; i < 16; ++i) result[w][i] = kCurves[w][i] >= 8 ? 15 : 0;
    return result;
  }();
  if (!SETTINGS.textAntiAliasing) return kBinaryCurves[weight < 5 ? weight : 2].data();
#endif
  return weight < std::size(kCurves) && weight != 2 ? kCurves[weight] : nullptr;
}

// Edges alone stop short of a heavy stroke (a stroke's core is already full ink), so the
// heaviest step also grows every stroke by a pixel (ink +54% over the font as drawn, against
// +19% for the edges alone). A half-pixel spread read as a shadow, not as weight.
uint8_t readerTextWeightSpread(const uint8_t weight) { return weight == 4 ? 2 : 0; }
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Protected-book sidecars travel with the book; a protected book separated
  // from its key no longer opens, so a failed move rolls everything back.
  static constexpr const char* SIDECARS[] = {".key", ".rights"};
  for (size_t i = 0; i < std::size(SIDECARS); i++) {
    const std::string from = srcPath + SIDECARS[i];
    if (!Storage.exists(from.c_str())) continue;
    const std::string to = dstPath + SIDECARS[i];
    if (Storage.rename(from.c_str(), to.c_str())) continue;
    LOG_ERR("ERS", "Failed to move sidecar %s -> %s", from.c_str(), to.c_str());
    for (size_t j = 0; j < i; j++) {
      Storage.rename((dstPath + SIDECARS[j]).c_str(), (srcPath + SIDECARS[j]).c_str());
    }
    if (!Storage.rename(dstPath.c_str(), srcPath.c_str())) {
      LOG_ERR("ERS", "Failed to restore epub after sidecar move failure: %s -> %s", dstPath.c_str(), srcPath.c_str());
    }
    return;
  }

  const bool artifactsOk = relocateBookArtifacts(srcPath, dstPath);
  const bool referencesOk = relocateBookReferences(srcPath, dstPath);
  if (!artifactsOk || !referencesOk) {
    LOG_ERR("ERS", "Finished-book data migration incomplete: %s -> %s", srcPath.c_str(), dstPath.c_str());
  }
}

int getChapterTocIndex(const Epub& epub, const int spineIndex, std::optional<uint32_t> visibleOffset,
                       uint8_t* chapterProgress = nullptr) {
  if (Txt::isTxtOrMd(epub.getPath()) && visibleOffset) {
    const int index = Txt::tocIndexForPosition(epub.getPath(), epub.getCachePath(), *visibleOffset, chapterProgress);
    if (index >= 0 && index < epub.getTocItemsCount()) return index;
  }
  return epub.getTocIndexForSpineIndex(spineIndex);
}

std::string getStatsChapterTitle(const Epub& epub, const int spineIndex, std::optional<uint32_t> visibleOffset,
                                 uint8_t* chapterProgress = nullptr) {
  int tocIndex = getChapterTocIndex(epub, spineIndex, visibleOffset, chapterProgress);
  if (tocIndex < 0) {
    int nearestTocIndex = -1;
    int nearestSpineIndex = -1;
    for (int index = 0; index < epub.getTocItemsCount(); ++index) {
      const int tocSpineIndex = epub.getSpineIndexForTocIndex(index);
      if (tocSpineIndex <= spineIndex && tocSpineIndex >= nearestSpineIndex) {
        nearestSpineIndex = tocSpineIndex;
        nearestTocIndex = index;
      }
    }
    tocIndex = nearestTocIndex;
  }
  return tocIndex < 0 ? "" : epub.getTocItem(tocIndex).title;
}

uint8_t getStatsChapterProgressPercent(const int currentPage, const int pageCount) {
  if (pageCount <= 0) return 0;
  return static_cast<uint8_t>(clampPercent(
      static_cast<int>((static_cast<float>(currentPage + 1) / static_cast<float>(pageCount)) * 100.0f + 0.5f)));
}

}  // namespace

bool EpubReaderActivity::needsPartialRebuild() const {
  return section && !section->isBuilding() && section->isPartial() && failedBuildSpine_ != currentSpineIndex &&
         section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount);
}

bool EpubReaderActivity::deferBluetoothStart() const {
  // Preparation blocks a new host, not an existing reader/menu connection.
  return !section || section->isBuilding() || section->pageCount == 0 || needsPartialRebuild();
}

void EpubReaderActivity::prepareChapterBuild() {
  // C3 and PSRAM hosts retain existing links; parser memory guards still apply.
#if FREEINK_CAP_BLE_HID_HOST && !CROSSPOINT_BLE_HOST_PSRAM && !CONFIG_IDF_TARGET_ESP32C3
  bleinput::stop();
#endif
#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
  // Reclaim arenas before parser allocations interleave with them; retain the
  // selected font and coverage index for both layout and subsequent reading.
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
#endif
}

bool EpubReaderActivity::updateChapterBuild() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return true;
  if (!section) return true;

  if (needsPartialRebuild() && buildViewportWidth > 0) {
    const ReaderRenderSpec buildSpec = effectiveRenderSpec(buildViewportWidth, buildViewportHeight);
    prepareChapterBuild();
    if (!section->startBuild(buildSpec)) {
      if (!handleBuildFailure("deferred partial start", section->buildError())) requestUpdate();
      return false;
    }
    LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
            section->pageCount);
  }
  if (!section->isBuilding()) return true;

  bool suspendForBluetooth = false;
#if CONFIG_IDF_TARGET_ESP32C3 && FREEINK_CAP_BLE_HID_HOST
  suspendForBluetooth = SETTINGS.bluetoothEnabled;
#endif
  // A paused parser still owns its heap. Build beyond the restart margin, then
  // persist a partial cache and release the parser to bound its lifetime.
  const int buildWindow = BUILD_WINDOW_AHEAD + (suspendForBluetooth ? PARTIAL_REBUILD_START_MARGIN : 0);
  const bool pendingReposition = cachedVisibleTextOffset.has_value() || cachedChapterTotalPageCount != 0;
  if (((!suspendForBluetooth && section->isPartial()) || (suspendForBluetooth && pendingReposition) ||
       static_cast<int>(section->pageCount) < section->currentPage + buildWindow) &&
      buildTickHeapGate()) {
    if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
      if (!handleBuildFailure("background build", section->buildError())) requestUpdate();
      return false;
    }
    if (section->isBuildComplete() && applyDeferredReposition()) requestUpdate();
  }

  if (suspendForBluetooth && section->isBuilding() && !pendingReposition &&
      static_cast<int>(section->pageCount) >= section->currentPage + buildWindow) {
    section->suspendBuild();
    if (section->buildError() != Section::BuildError::None) {
      if (!handleBuildFailure("partial suspend", section->buildError())) requestUpdate();
      return false;
    }
  }
  return true;
}

EpubReaderActivity::~EpubReaderActivity() {
  ImageBlock::setExtractor(nullptr, nullptr);
  settleOverlayRefresh();
  discardOverlayPage();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::onExit() {
#if FREEINK_DEVICE_METALIO_PLUS
  // Finish any display borrower before releasing its reader-owned buffers.
  // ActivityManager already holds RenderLock and has cancelled idle rendering.
  renderer.waitRefreshComplete();
  settleOverlayRefresh();
  discardOverlayPage();
  ImageBlock::releaseRenderCache();
#endif
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  READING_STATS.endSession();
  ACHIEVEMENTS.recordSessionEnded(READING_STATS.getLastSessionSnapshot());
  reading_excerpt::persist();
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  // Four frames of PSRAM; the reader is the only owner and gives them back here.
  freePageCache();
#endif
  showPendingAchievementPopups(renderer);
#if RICKYOS_PRODUCT
  rickyBadgeNotice::announceNew(renderer);
#endif
  bookFonts_.reset();  // unregister book faces before the reader font is released
  ReaderActivity::onExit();
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  // The last page is 16-level; a fast B/W frame over it leaves its gray text behind,
  // and the UI's later refreshes only drive what they change, so the residue stayed and
  // built up. Clean the completed destination frame, not the outgoing book page.
  // Direct-gray UI honors this request too and suppresses its turn animation.
  renderer.requestNextFullRefresh();
#endif
}

void EpubReaderActivity::showOpeningPopup() {
  // Even a cached book takes a few seconds on device to its first page (measured
  // 2.7-3.6 s), with the tapped screen unchanged meanwhile. Say so at once.
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
  // Drawn into the retained 16-level frame: the screen keeps its grays and only the
  // popup's area is driven, instead of a B/W refresh of the whole screen.
  if (renderer.beginRetainedGrayscale16()) {
    renderer.setDisplayCapture(true);
    GUI.drawPopup(renderer, tr(STR_RICKY_OPENING_BOOK));
    renderer.setDisplayCapture(false);
    renderer.commitGrayscale16();
    renderer.cancelGrayscale16();
    return;
  }
#endif
  GUI.drawPopup(renderer, tr(STR_RICKY_OPENING_BOOK));
}

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
#if defined(BOARD_HAS_PSRAM) || defined(SIMULATOR)
  constexpr bool kLoadProgress = true;  // see the loan below
#else
  constexpr bool kLoadProgress = false;
#endif
  const auto preparingPopup = [this] {
    return kLoadProgress ? GUI.drawProgressPopup(renderer, tr(STR_RICKY_PREPARING_BOOK))
                         : GUI.drawPopup(renderer, tr(STR_RICKY_PREPARING_BOOK));
  };
  Rect popup{};
  if (uncached) {
#if FREEINK_CAP_BLE_HID_HOST && !CROSSPOINT_BLE_HOST_PSRAM && !CONFIG_IDF_TARGET_ESP32C3
    bleinput::stop();
#endif
    disableFastInitialRefresh();
    popup = preparingPopup();
  } else if (SETTINGS.embeddedStyle != 0 &&
             !Storage.exists((loadedEpub->getCachePath() + "/css_rules.cache").c_str())) {
    // A book without its style cache parses every stylesheet once (seconds for books
    // with hundreds of them): say so instead of looking frozen.
    popup = preparingPopup();
  } else {
    showOpeningPopup();
  }

  bool loaded;
  {
#if defined(BOARD_HAS_PSRAM) || defined(SIMULATOR)
    // With PSRAM the build does not need the framebuffer's bytes, so the popup keeps it
    // and shows progress: a first open of a large book takes many seconds on device.
    struct Progress {
      const GfxRenderer& renderer;
      Rect popup;
      int shown = 0;
      unsigned long shownAt = 0;
    } progress{renderer, popup, 0, millis()};
    if (popup.width > 0) {
      loadedEpub->setLoadProgress(
          [](void* context, const int percent) {
            auto& state = *static_cast<Progress*>(context);
            // Each update is a panel refresh: only for visible steps, at most every 1.2 s.
            if (percent < state.shown + 5 || millis() - state.shownAt < 1200) return;
            state.shown = percent;
            state.shownAt = millis();
            GUI.fillPopupProgress(state.renderer, state.popup, percent);
          },
          &progress);
    }
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached && popup.width <= 0) loan.emplace(renderer);
#else
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
#endif
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
    loadedEpub->setLoadProgress(nullptr, nullptr);
  }
  if (!loaded) {
    // Surfaced by handleLoadFailure() as a dialog; loadedEpub dies with this
    // scope, so carry the reason out in a member.
    loadProtectionError = loadedEpub->getProtectionError();
    LOG_ERR("ERS", "Failed to load EPUB%s%s", loadProtectionError.empty() ? "" : ": ", loadProtectionError.c_str());
    return false;
  }
  epub = std::move(loadedEpub);

  bookFonts_.reset();
  if (SETTINGS.embeddedStyle) {
    bookFonts_ = makeUniqueNoThrow<BookFontSet>(renderer);
    if (bookFonts_) {
      bookFonts_->open(*epub);
      bookfonts::setActive(bookFonts_.get());
    } else {
      LOG_ERR("BOOKFONT", "Cannot allocate book font set; using reader font");
    }
  }

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest, CancelCheck cancellation) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest, cancellation);
  });

  epub->setupCacheDir();

  wereadBookId_[0] = '\0';
  if (WeReadStore::findBookIdForPath(epub->getPath(), wereadBookId_, sizeof(wereadBookId_)) &&
      strncmp(wereadBookId_, "MP_WXS_", 7) == 0) {
    wereadBookId_[0] = '\0';
  }
  if (wereadBookId_[0]) {
    const uint32_t timestamp = TimeUtils::getCurrentValidTimestamp();
    if (timestamp != 0) {
      switch (WeReadStore::promoteShelfBook(wereadBookId_, timestamp)) {
        case WeReadStore::ShelfSortResult::Ok:
          break;
        case WeReadStore::ShelfSortResult::Degraded:
          LOG_DBG("WR", "Large shelf: recent book promotion deferred until sync");
          break;
        case WeReadStore::ShelfSortResult::StorageError:
          LOG_ERR("WR", "Failed to promote recently opened shelf book");
          break;
      }
    }
  }
  bool hasSavedProgress = false;

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      hasSavedProgress = true;
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  uint32_t markdownOffset = 0;
  const auto markdownProgress = Txt::restoreMarkdownProgress(bookPath, epub->getCachePath(), markdownOffset);
  if (markdownProgress == txt_progress::LegacyResult::Failed) {
    legacyProgressPending = true;
    loadProtectionError = "markdown progress migration failed";
    return false;
  }
  if (markdownProgress == txt_progress::LegacyResult::Restored) {
    currentSpineIndex = cachedSpineIndex = nextPageNumber = 0;
    cachedChapterTotalPageCount = 0;
    cachedVisibleTextOffset = pendingOffsetJump = markdownOffset;
    hasSavedProgress = true;
    legacyProgressPending = markdownProgressPending_ = true;
  }

  if (!hasSavedProgress && Txt::isTxtOrMd(bookPath)) {
    uint32_t visibleOffset = 0;
    switch (Txt::restoreLegacyProgress(bookPath, epub->getCachePath(), visibleOffset)) {
      case txt_progress::LegacyResult::Absent:
        break;
      case txt_progress::LegacyResult::Restored:
        currentSpineIndex = cachedSpineIndex = 0;
        nextPageNumber = 0;
        cachedVisibleTextOffset = pendingOffsetJump = visibleOffset;
        legacyProgressPending = true;
        break;
      case txt_progress::LegacyResult::Failed:
        legacyProgressPending = true;
        loadProtectionError = "txt progress migration failed";
        return false;
    }
  }

  // A split TXT: the saved offset names the part (its parts may differ from when it was
  // saved); without one, an out-of-range part starts the book rather than its end.
  if (Txt::usesParts(bookPath)) {
    const int part = cachedVisibleTextOffset ? epub->txtPartForVisibleOffset(*cachedVisibleTextOffset) : -1;
    if (part >= 0) {
      if (part != currentSpineIndex) nextPageNumber = 0;
      currentSpineIndex = cachedSpineIndex = part;
    } else if (currentSpineIndex >= epub->getSpineItemsCount()) {
      currentSpineIndex = cachedSpineIndex = 0;
      nextPageNumber = 0;
      cachedChapterTotalPageCount = 0;
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  if (wereadBookId_[0]) {
    float initialProgress = 0.0f;
    const bool loaded = WeReadStore::loadInitialProgress(wereadBookId_, initialProgress);
    if (hasSavedProgress || !loaded || initialProgress <= 0.0f) {
      WeReadStore::clearInitialProgress(wereadBookId_);
    } else if (jumpToFraction(initialProgress)) {
      clearInitialProgressAfterSave_ = true;
    } else {
      WeReadStore::clearInitialProgress(wereadBookId_);
    }
  }

  READING_STATS.beginSession(
      epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getCoverBmpPath(),
      clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, 0.0f) * 100.0f + 0.5f)),
      getStatsChapterTitle(*epub, currentSpineIndex, cachedVisibleTextOffset), 0);

  loadLinkStack();
  loadCachedBookmarks();
  return true;
}

ChapterPosition EpubReaderActivity::chapterPosition() const {
  if (section) return {section->currentPage, section->estimatedTotalPages()};
  return {nextPageNumber, cachedChapterTotalPageCount};
}

int EpubReaderActivity::bookPercentFor(const ChapterPosition& position) const {
  if (!epub || epub->getBookSize() == 0 || !position.hasTotal()) return 0;
  // The page index can run past the chapter's estimated total while it is still
  // building, so the fraction is clamped before the cast.
  const float fraction = epub->calculateProgress(currentSpineIndex, position.chapterFraction());
  return static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
    if (!toolbarUi) {
      LOG_ERR("ERS", "OOM allocating reader toolbar");
      overlay = Overlay::None;
      return;
    }
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  const ChapterPosition position = chapterPosition();
  const int currentPage = position.displayPage();
  const int totalPages = position.totalPages;
  const int bookProgressPercent = bookPercentFor(position);
  const uint8_t readingModeBefore = SETTINGS.readingMode;
  const uint8_t guideBefore =
      static_cast<uint8_t>(SETTINGS.readingGuideLineEnabled << 4 | SETTINGS.readingGuideLineStyle);
  const uint8_t sizeBefore = SETTINGS.fontPointSize;
  const uint8_t weightBefore = SETTINGS.rickyTextWeight;
  startActivityForResultWith<EpubReaderMenuActivity>(
      [this, readingModeBefore, guideBefore, sizeBefore, weightBefore](const ActivityResult& result) {
        READING_STATS.resumeSession();
        const auto& menu = std::get<MenuResult>(result.data);
        if (SETTINGS.orientation != menu.orientation) {
          applyOrientation(menu.orientation);
        }
        // The menu saved a new reading mode: its frame and bands move the text column.
        if (SETTINGS.readingMode != readingModeBefore) {
          applyReaderTextSettings();
          requestUpdate();
        }
        // Reading lines are drawn, not laid out: the page cache key changes with them.
        if ((SETTINGS.readingGuideLineEnabled << 4 | SETTINGS.readingGuideLineStyle) != guideBefore) requestUpdate();
        // 字重 is drawn too; 字号 re-paginates and may change which font file is in use.
        if (SETTINGS.rickyTextWeight != weightBefore) {
          discardOverlayPage();
          requestUpdate();
        }
        // Only a plain close may open the acceleration prompt; a picked action goes first.
        if (SETTINGS.fontPointSize != sizeBefore) applyFontSizeFromMenu(result.isCancelled);
        toggleAutoPageTurn(menu.pageTurnRate);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      },
      epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
      !currentPageFootnotes.empty(), !cachedBookmarks.empty(), currentChapterTitle().c_str(), currentPageBookmarked,
      static_cast<unsigned>(cachedBookmarks.size()));
}

ReaderRenderSpec EpubReaderActivity::effectiveRenderSpec(const uint16_t width, const uint16_t height) const {
  const bool vertical = readingMode::setsVertically(readingMode::modeFor(SETTINGS.readingMode));
  // Vertical setting lays its columns out as lines, so the layout sees the viewport turned.
  auto spec = vertical ? SETTINGS.readerRenderSpec(height, width) : SETTINGS.readerRenderSpec(width, height);
  spec.collectTouchLinks = mappedInput.hasTouch() && !vertical;
  if (stylesDisabledForSession_) spec.embeddedStyle = false;
  spec.bookFontsKey = spec.embeddedStyle && bookFonts_ ? bookFonts_->key() : 0;
#if FREEINK_DEVICE_METALIO_PLUS
  const auto mode = readingMode::modeFor(SETTINGS.readingMode);
  if (mode == readingMode::Mode::Original || mode == readingMode::Mode::Immersive) {
    readingMode::Margins viewable;
    renderer.getOrientedViewableTRBL(&viewable.top, &viewable.right, &viewable.bottom, &viewable.left);
    const auto text = textMargins();
    spec.imageOutset = plusReaderImage::horizontalOutset(text.left, text.right, viewable.left, viewable.right);
  }
#endif
  if (vertical) {
    spec.verticalText = true;
    // Columns keep one pitch so the 界行 stay on their grid; images have no column
    // form yet and show their alt text.
    spec.extraParagraphSpacing = 0;
    spec.imageRendering = 1;
  }
  return spec;
}

namespace {
// Readers cannot send serial logs, so a failed chapter leaves one line on the card they
// can copy off: firmware, book, chapter, stage, cause and the heap at that moment.
constexpr char kRickyErrorLogPath[] = "/rickyos-log.txt";
constexpr size_t kRickyErrorLogMaxBytes = 32 * 1024;

void appendRickyErrorLog(const std::string& bookPath, const int spine, const char* stage, const unsigned error) {
  char line[96];
  const int head = snprintf(
      line, sizeof(line), "%s up=%lus spine=%d stage=%s error=%u free=%u min=%u max=%u book=", CROSSPOINT_VERSION,
      static_cast<unsigned long>(millis() / 1000), spine, stage, error, static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (head <= 0) return;
  std::string entry(line, std::min(static_cast<size_t>(head), sizeof(line) - 1));
  entry += bookPath;
  entry += '\n';
  HalFile file = Storage.open(kRickyErrorLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (file && file.isOpen() && file.fileSize() > kRickyErrorLogMaxBytes) {
    file.close();  // keep the card tidy: start over rather than grow without bound
    Storage.remove(kRickyErrorLogPath);
    file = Storage.open(kRickyErrorLogPath, O_WRONLY | O_CREAT | O_APPEND);
  }
  if (!file || !file.isOpen()) return;
  file.write(reinterpret_cast<const uint8_t*>(entry.data()), entry.size());
  file.flush();
  file.close();
}
}  // namespace

bool EpubReaderActivity::handleBuildFailure(const char* stage, const Section::BuildError error) {
  if (failedBuildSpine_ == currentSpineIndex) return false;
  bool retry = false;
#ifndef BOARD_HAS_PSRAM
  retry = error == Section::BuildError::OutOfMemory && !stylesDisabledForSession_ && SETTINGS.embeddedStyle != 0;
#endif
  if (retry) {
    // A page number is not a content position after CSS changes. Explicit
    // anchor/percentage/offset requests retain precedence over the displayed page.
    if (pendingPageJump == std::numeric_limits<uint16_t>::max()) {
      pendingPercentJump = true;
      pendingSpineProgress = 1.0f;
    }
    if (pendingAnchor.empty() && !pendingPercentJump && !pendingOffsetJump) {
      if (renderedSpineIndex_ == currentSpineIndex && currentPageVisibleOffset) {
        pendingOffsetJump = currentPageVisibleOffset;
      } else if (cachedSpineIndex == currentSpineIndex && cachedVisibleTextOffset) {
        pendingOffsetJump = cachedVisibleTextOffset;
      }
    }
    pendingPageJump.reset();
    clearDeferredReposition();
    nextPageNumber = 0;
    stylesDisabledForSession_ = true;
  }
  if (section) section->abandonBuild();
  section.reset();
  if (auto* css = epub->getCssParser()) css->clear();
  discardOverlayPage();
  currentPageLinks.clear();
  currentPageFootnotes.resize(0);
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  buildHeapPaused = false;
  buildPopupPending = false;
  failedBuildSpine_ = retry ? -1 : currentSpineIndex;
  failedBuildError_ = error;
  appendRickyErrorLog(epub->getPath(), currentSpineIndex, stage, static_cast<unsigned>(error));
  LOG_ERR("ERS", "Build failed: spine=%d stage=%s error=%u retryWithoutCss=%d (free=%u, min=%u, maxAlloc=%u)",
          currentSpineIndex, stage, static_cast<unsigned>(error), retry, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (retry) {
    requestUpdate();
  } else {
    automaticPageTurnActive = false;
  }
  return retry;
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
#ifndef BOARD_HAS_PSRAM
  if (buildHeapPaused && !stylesDisabledForSession_ && SETTINGS.embeddedStyle != 0) return true;
#endif
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

bool EpubReaderActivity::refuseVerticalSelect() {
  // Word and footnote pickers hit-test horizontal lines; columns need their own geometry.
  if (!readingMode::setsVertically(readingMode::modeFor(SETTINGS.readingMode))) return false;
  showDictionaryMessage = true;
  dictionaryMessageVertical = true;
  dictionaryMessageTime = millis();
  requestUpdate();
  return true;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (refuseVerticalSelect()) return;
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageVertical = false;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  const readingMode::Margins margins = textMargins();
  const int orientedMarginTop = margins.top;
  const int orientedMarginLeft = margins.left;

  startActivityForResultWith<DictionaryWordSelectActivity>(
      [this](const ActivityResult&) {
        READING_STATS.resumeSession();
        requestUpdate();
      },
      std::move(page), orientedMarginLeft, orientedMarginTop);
}

bool EpubReaderActivity::maybeOfferCompleteChineseFont() {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    pendingMissingChineseCodepoint_.store(0, std::memory_order_relaxed);
    return false;
  }

  const uint32_t codepoint = pendingMissingChineseCodepoint_.exchange(0, std::memory_order_relaxed);
  if (codepoint == 0 || FontDownloadActivity::wasChineseFontPromptShownThisBoot()) return false;

  LOG_INF("FONT", "Missing built-in Chinese glyph U+%04X; offering automatic NotoSansSC install",
          static_cast<unsigned>(codepoint));
  auto downloader =
      makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInput, FontDownloadActivity::Purpose::ReaderAutoInstall);
  if (!downloader) {
    LOG_ERR("FONT", "OOM allocating FontDownloadActivity (%zu bytes)", sizeof(FontDownloadActivity));
    return false;
  }
  startActivityForResult(std::move(downloader), [this](const ActivityResult&) {
    READING_STATS.resumeSession();
    requestUpdate();
  });
  return true;
}
void EpubReaderActivity::openFootnoteSelect(const bool reopenMenuOnCancel) {
  if (!section || currentPageFootnotes.empty()) return;
  if (currentPageFootnotes.size() == 1) {
    navigateToHref(currentPageFootnotes[0].href, true);
    return;
  }
  if (refuseVerticalSelect()) return;

  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  const readingMode::Margins margins = textMargins();
  const int orientedMarginTop = margins.top;
  const int orientedMarginLeft = margins.left;
  auto selector = makeUniqueNoThrow<EpubReaderFootnoteSelectActivity>(renderer, mappedInput, std::move(page),
                                                                      orientedMarginLeft, orientedMarginTop);
  if (!selector) {
    LOG_ERR("ERA", "OOM: EpubReaderFootnoteSelectActivity");
    return;
  }
  startActivityForResult(std::move(selector), [this, reopenMenuOnCancel](const ActivityResult& result) {
    READING_STATS.resumeSession();
    if (result.isCancelled) {
      if (reopenMenuOnCancel) {
        openReaderMenu();
      } else {
        requestUpdate();
      }
      return;
    }
    const auto& footnoteResult = std::get<FootnoteResult>(result.data);
    navigateToHref(footnoteResult.href, true);
    requestUpdate();
  });
}

void EpubReaderActivity::loop() {
  if (loadFailurePopup.isActive()) {
    loadFailurePopup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  if (!epub) {
    finish();
    return;
  }

  if (fontPromptState != FontPromptState::Idle) {
    lastPageTurnTime = millis();
    handleFontPreloadPrompt();
    return;
  }
  // Child activities suspend this loop; their temporary Overlay::None is not
  // a return to reading. Panel/tool switches retain the same preview session.
  if (overlay == Overlay::None && fontPreview.active()) {
    lastPageTurnTime = millis();
    finishFontPreview();
    return;
  }

  if (maybeOfferCompleteChineseFont()) return;

  READING_STATS.tickActiveSession();
  rememberBookOnceRendered();

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

#if !FREEINK_DEVICE_READPICO && !FREEINK_DEVICE_METALIO_PLUS
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  auto* fcm = renderer.getFontCacheManager();
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock() && section && !section->isBuilding() && renderer.hasFrameBuffer() &&
        lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
        ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC && fcm &&
        fcm->canIdlePrewarm(SETTINGS.getReaderFontId()) &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          const auto t0 = millis();
          auto scope = fcm->createPrewarmScope();
          p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
          scope.endScanAndPrewarm();
          LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
        }
      }
    }
  }

#endif

  if (!updateChapterBuild()) return;

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch =
      ReaderUtils::detectTouchPageTurn(renderer, mappedInput, ReaderUtils::isRtlBookLanguage(epub->getLanguage()));

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  switch (mappedInput.homeButtonAction()) {
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Sync:
    case HomeButtonAction::Dictionary:
    case HomeButtonAction::Footnotes:
      automaticPageTurnActive = false;
      break;
    default:
      break;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      const bool succeeded = pageTurn(true);
      notePageTurn(true, succeeded);
      requestUpdate();
      return;
    }
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (wereadBookId_[0] && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && launchWeReadSync()) return;
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && launchKOReaderSync()) return;
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          openDictionaryWordSelect();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (!endOfBookMenuOpen) {
    switch (mappedInput.homeButtonAction()) {
      case HomeButtonAction::Bookmark:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case HomeButtonAction::Sync:
        if (wereadBookId_[0]) {
          launchWeReadSync();
          return;
        }
        launchKOReaderSync();
        return;
      case HomeButtonAction::Dictionary:
        if (!showDictionaryMessage) openDictionaryWordSelect();
        return;
      case HomeButtonAction::ReaderMenu:
        if (usesToolbarMenu() && section)
          openOverlay(Overlay::Toolbar);
        else
          openReaderMenu();
        return;
      default:
        break;
    }
  }

  // Link taps take priority over the reader-menu and page-turn zones.
  if (!atEndOfBook && !currentPageLinks.empty() && SETTINGS.touchReaderControls && mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      const auto* link = EpubReaderUtils::linkAtPoint(currentPageLinks, touchX, touchY, currentPageLinkMarginLeft,
                                                      currentPageLinkMarginTop);
      if (link) {
        navigateToHref(link->href, true);
        return;
      }
    }
  }

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if ((!endOfBookMenuOpen && mappedInput.homeButtonAction() == HomeButtonAction::Footnotes) ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
       mappedInput.wasReleased(MappedInputManager::Button::Power) &&
       !mappedInput.wasReleased(MappedInputManager::Button::Down))) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      openFootnoteSelect(false);
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      pendingManualTurn = 0;
      return;
    }
    const auto request = pendingManualTurn;
    const bool forward = nativePageTurn::direction(request) > 0;
    pendingManualTurn = 0;
    const bool succeeded = pageTurnWithOrigin(forward, nativePageTurn::originX(request), nativePageTurn::originY(request));
    notePageTurn(forward, succeeded);
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    const bool succeeded = skipPages(nextTriggered ? 1 : -1);
    notePageTurn(false, succeeded);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  int originX = -1, originY = -1;
  int touchX = 0, touchY = 0;
  // Origin and direction come from the same immutable input snapshot.
  if ((touch.prev || touch.next) && mappedInput.wasScreenTapped(touchX, touchY)) {
    originX = nativePageTurn::normalize(touchX, renderer.getScreenWidth());
    originY = nativePageTurn::normalize(touchY, renderer.getScreenHeight());
  }
  if (turnGuardActive) {
    // While the idle pass is building the next page, let that page finish (a soft,
    // touch-class cancel stops everything after it): the deferred turn then uses it.
    activityManager.cancelIdleRender(activityManager.touchTolerantIdleActive() ? "touch" : "turn guard");
    pendingManualTurn = nativePageTurn::pack(prevTriggered ? -1 : 1, originX, originY);
    return;
  }

  if (prevTriggered) {
    const bool succeeded = pageTurnWithOrigin(false, originX, originY);
    notePageTurn(false, succeeded);
  } else {
    const bool succeeded = pageTurnWithOrigin(true, originX, originY);
    notePageTurn(true, succeeded);
  }
  requestUpdate();
}

bool EpubReaderActivity::jumpToFraction(float fraction) {
  if (!epub || !std::isfinite(fraction)) return false;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return false;
  fraction = std::clamp(fraction, 0.0f, 1.0f);

  const size_t targetSize =
      fraction >= 1.0f ? bookSize - 1 : static_cast<size_t>(static_cast<double>(bookSize) * fraction);

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return false;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
  return true;
}

void EpubReaderActivity::jumpToPercent(const int percent) {
  jumpToFraction(static_cast<float>(clampPercent(percent)) / 100.0f);
}

// Move to a position picked elsewhere (a bookmark, a sync): the exact text offset when
// the source has one, else the page hints, else the saved xpath/percentage.
void EpubReaderActivity::applyProgressChange(const ProgressChangeResult& sync) {
  if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
    RenderLock lock;
    clearDeferredReposition();
    if (section && currentSpineIndex == sync.spineIndex) {
      const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
      section->currentPage = page.value_or(std::max(0, sync.page));
    } else {
      currentSpineIndex = sync.spineIndex;
      pendingOffsetJump = sync.visibleTextOffset;
      nextPageNumber = std::max(0, sync.page);
      section.reset();
    }
    requestUpdate();
    return;
  }

  int targetSpineIndex = sync.spineIndex;
  int targetPage = sync.page;
  const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
  const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 && currentSpineIndex == sync.spineIndex &&
                                              sync.page >= 0 && sync.page < sync.totalPages &&
                                              activeTotalPages == sync.totalPages;

  if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
    const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
    CrossPointPosition fallback =
        ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
    targetSpineIndex = fallback.spineIndex;
    targetPage = fallback.pageNumber;
  }

  RenderLock lock;
  clearDeferredReposition();

  if (currentSpineIndex != targetSpineIndex) {
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = targetPage;
    section.reset();
  } else if (section && section->currentPage != targetPage) {
    const int clampedTargetPage = std::max(0, targetPage);
    section->currentPage = clampedTargetPage;
  } else if (!section) {
    nextPageNumber = targetPage;
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    READING_STATS.resumeSession();
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      applyProgressChange(std::get<ProgressChangeResult>(result.data));
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const int tocIdx = currentTocIndex();
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      startActivityForResultWith<EpubReaderChapterSelectionActivity>(
          [this](const ActivityResult& result) {
            READING_STATS.resumeSession();
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          },
          epub, spineIdx, tocIdx);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      openFootnoteSelect(true);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResultWith<TextSettingsActivity>(
          [this](const ActivityResult&) {
            READING_STATS.resumeSession();
            applyReaderTextSettings();
            openReaderMenu();
          },
          &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      // Both open option popups inside EpubReaderMenuActivity and only reach
      // the result callback after another action closes the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = bookPercentFor(chapterPosition());
      startActivityForResultWith<EpubReaderPercentSelectionActivity>(
          [this](const ActivityResult& result) {
            READING_STATS.resumeSession();
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          },
          initialPercent);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResultWith<QrDisplayActivity>(
              [this](const ActivityResult&) {
                READING_STATS.resumeSession();
                openReaderMenu();
              },
              fullText);
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          // Keep the exact position: the save below runs after the section is gone, and a
          // split TXT may come back with different parts, so its page number alone is lost.
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (wereadBookId_[0]) {
        launchWeReadSync();
        break;
      }
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResultWith<EpubReaderBookmarksActivity>(progressChangeResultHandler, epub, epub->getPath());
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      showBookmarkMessage = true;
      bookmarkMessageTime = millis();
      break;
    }
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
      if (wereadBookId_[0]) return ReaderUtils::GO_HOME_MS;
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  RenderLock renderLock;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos;
  std::string localChapterName = currentChapterTitle();
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    if (section) {
      nextPageNumber = section->currentPage;
    }
    discardOverlayPage();
    ImageBlock::releaseRenderCache();
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseSdFontCaches();
    }
    // No rendering may run while the chapter mapper borrows the framebuffer.
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
    }
    // The destructor can no longer save the back-stack once epub is gone.
    if (footnoteDepth > 0) saveLinkStack();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  return activityManager.replaceActivityWith<KOReaderSyncActivity>(savedEpubPath, localPos, std::move(localKoPos),
                                                                   std::move(localChapterName));
}

bool EpubReaderActivity::launchWeReadSync() {
  if (!wereadBookId_[0]) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  const float chapterFraction =
      totalPages > 1 ? static_cast<float>(currentPage) / static_cast<float>(totalPages - 1) : 0.0f;
  const float localFraction = epub->calculateProgress(currentSpineIndex, chapterFraction);
  const CrossPointPosition localPosition = getCurrentPosition();
  std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("WRSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  const auto context = WeReadProgressSyncActivity::makeContext(*epub, wereadBookId_, localFraction, localPosition);
  auto sync = makeUniqueNoThrow<WeReadProgressSyncActivity>(renderer, mappedInput, std::move(savedEpubPath),
                                                            wereadBookId_, context);
  if (!sync) {
    LOG_ERR("WRSync", "OOM: WeReadProgressSyncActivity (%u bytes)",
            static_cast<unsigned>(sizeof(WeReadProgressSyncActivity)));
    pendingSyncLaunchError = true;
    requestUpdate();
    return true;
  }

  {
    RenderLock lock;
    if (section) nextPageNumber = section->currentPage;
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  activityManager.replaceActivity(std::move(sync));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  pendingManualTurn = 0;
  pendingNativeTurn_ = 0; // Do not reuse coordinates after an orientation change.
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t requestedPageTurnRate) {
  if (requestedPageTurnRate == 0 || requestedPageTurnRate > MAX_PAGE_TURN_RATE) {
    automaticPageTurnActive = false;
    return;
  }

  pageTurnRate = requestedPageTurnRate;
  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / pageTurnRate;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // The auto-turn label claims a status lane; modes without the status bar never show it.
  if (readingMode::showsFooter(readingMode::modeFor(SETTINGS.readingMode)) &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  return pageTurnWithOrigin(isForwardTurn, -1, -1);
}

bool EpubReaderActivity::pageTurnWithOrigin(bool isForwardTurn, int originX, int originY) {
  pendingNativeTurn_ = 0;
  if (!section) return false;
  READING_STATS.noteActivity();
#if RICKYOS_PLUS_READER_TIMING
  const auto tTurnInput = millis();
#endif
  {
    RenderLock lock;
    clearDeferredReposition();
  }
#if RICKYOS_PLUS_READER_TIMING
  LOG_INF("ReaderTiming", "turn input at=%lu lockwait=%lums", static_cast<unsigned long>(tTurnInput),
          static_cast<unsigned long>(millis() - tTurnInput));
#endif
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
pendingNativeTurn_ = nativePageTurn::pack(1, originX, originY);
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      lastPageTurnTime = millis();
      return true;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
pendingNativeTurn_ = nativePageTurn::pack(-1, originX, originY);
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::skipPages(int amount) {
  pendingNativeTurn_ = 0;
  if (!section) return false;
  READING_STATS.noteActivity();
  if (amount > 0) {
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    section.reset();
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      return true;
    }
  }
  return false;
}

// Failed protected open: show the standard option dialog (wrapped message)
// instead of silently falling back to the previous screen. Exact error
// strings are set by openProtectedBook (ContentProtection.cpp).
bool EpubReaderActivity::handleLoadFailure() {
  if (loadProtectionError.empty()) return false;
  const std::string& perr = loadProtectionError;
  StrId msg = StrId::STR_DRM_PROTECTED_FILE;
  bool offerSync = false;
  if (perr == "txt progress migration failed") {
    msg = StrId::STR_TXT_PROGRESS_MIGRATION_FAILED;
  } else if (perr == "kindle memory") {
    msg = StrId::STR_MEMORY_ERROR;
  } else if (perr == "kindle unsupported") {
    msg = StrId::STR_RICKY_NO_PREVIEW;
  } else if (perr == "kindle invalid") {
    msg = StrId::STR_PAGE_LOAD_ERROR;
  } else if (perr == "access expired") {
    msg = StrId::STR_LOAN_EXPIRED;
  } else if (perr == "loan date unverified") {
    msg = StrId::STR_LOAN_TIME_UNVERIFIED;
    offerSync = true;
  }
  const char* options[2] = {I18N.get(offerSync ? StrId::STR_CLOCK_SYNC_NOW : StrId::STR_OK_BUTTON),
                            I18N.get(StrId::STR_OK_BUTTON)};
  loadFailurePopup.showMessage("", I18N.get(msg), options, offerSync ? 2 : 1, 0, [this, offerSync](const int index) {
    if (offerSync && index == 0) {
      beginLoanTimeSync();
      return;
    }
    finish();
  });
  requestUpdate();
  return true;  // stay alive; the popup's Back dismiss lands in loop()'s !epub finish
}

void EpubReaderActivity::beginLoanTimeSync() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ERS", "OOM: Wi-Fi selection for loan time sync");
    finish();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
      finish();
      return;
    }
    GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
    const bool synced = trustedtime::syncNow(5000);
    WiFi.disconnect(false);
    delay(30);
    if (!synced) {
      // Reopening would hit the same unverified-time refusal: offer a retry.
      const char* options[2] = {I18N.get(StrId::STR_RETRY), I18N.get(StrId::STR_OK_BUTTON)};
      loadFailurePopup.showMessage("", I18N.get(StrId::STR_CLOCK_SYNC_FAIL), options, 2, 0, [this](const int index) {
        if (index == 0) {
          beginLoanTimeSync();
          return;
        }
        finish();
      });
      requestUpdate();
      return;
    }
    APP_STATE.openEpubPath = bookPath;
    APP_STATE.saveToFile();
    // Reboot straight back into this book with a clean heap (no-op on touch
    // boards, which fall through to the in-place relaunch below).
    silentRestartToReader();
    activityManager.goToReader(bookPath);
  });
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::backgroundBuildWanted() const {
  return section && section->isBuilding() &&
         (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

void EpubReaderActivity::render(RenderLock&& lock) {
  if (loadFailurePopup.isActive()) {
    renderer.clearScreen();
    loadFailurePopup.processRender(renderer, mappedInput);
    return;
  }
  if (fontPromptState == FontPromptState::TooLarge) {
    fontpreload::drawTooLargeNotice(renderer);
    renderer.displayBuffer();
    return;
  }
  ReaderActivity::render(std::move(lock));
  // Rebuild the page underneath, including after a global control-center visit.
  if (fontPromptState != FontPromptState::Idle) {
    overlayPopup.processRender(renderer, mappedInput);
  }
}

bool EpubReaderActivity::skipLoopDelay() {
  // The main loop holds the render lock while querying this hint.
  return !buildHeapPaused && backgroundBuildWanted();
}

readingMode::Margins EpubReaderActivity::textMargins() const {
  readingMode::Margins viewable;
  renderer.getOrientedViewableTRBL(&viewable.top, &viewable.right, &viewable.bottom, &viewable.left);
  const int marginTop = SETTINGS.marginTop;
  const int marginBottom = SETTINGS.marginBottom;
  int marginLeft = SETTINGS.marginLeft;
  int marginRight = SETTINGS.marginRight;
  // Never squeeze the text column below half the page width, whatever the sides add up to.
  const int usableWidth = renderer.getScreenWidth() - viewable.left - viewable.right;
  const int sideLimit = std::max(0, usableWidth / 2);
  if (marginLeft + marginRight > sideLimit) {
    const int sum = marginLeft + marginRight;
    marginLeft = marginLeft * sideLimit / sum;
    marginRight = sideLimit - marginLeft;
  }
  const auto mode = readingMode::modeFor(SETTINGS.readingMode);
  const auto sb = SETTINGS.statusBarSpec();
  const bool textLane = sb.textLaneVisible();
  const auto chrome = UiHighDpiProfile::enabled
                          ? readerChrome::compactLayout(
                                std::max(renderer.getLineHeight(RickyDesignUi::CAPTION),
                                         renderer.getLineHeight(BaseTheme::STATUS_NUMERIC_FONT_ID)),
                                sb.headerLaneVisible(), sb.footerLaneVisible(), sb.showsProgressBar(),
                                sb.progressBarHeightPx)
                          : readerChrome::CompactLayout{};

  const bool headerLane = readingMode::showsHeader(mode) && UiHighDpiProfile::enabled && sb.headerLaneVisible();
  const int topFloor = viewable.top + (headerLane ? chrome.headerReserve : 0);
  int top = std::max(viewable.top + marginTop, topFloor);
  int bottomReserve = 0;
  if (readingMode::showsFooter(mode)) {
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    bottomReserve = statusBarHeight;
    if (automaticPageTurnActive &&
        (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
      bottomReserve += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
    if (UiHighDpiProfile::enabled && (!automaticPageTurnActive || textLane)) {
      bottomReserve = chrome.footerReserve;
    }
  }
  const int bottomFloor = viewable.bottom + bottomReserve;
  int bottom = std::max(viewable.bottom + marginBottom, bottomFloor);
  // The same for height: what the sides add beyond the header/footer reserve may
  // take at most half of the height left between them.
  const int room = std::max(0, (renderer.getScreenHeight() - topFloor - bottomFloor) / 2);
  const int extraTop = top - topFloor, extraBottom = bottom - bottomFloor;
  if (extraTop + extraBottom > room) {
    const int keptTop = extraTop * room / (extraTop + extraBottom);
    top = topFloor + keptTop;
    bottom = bottomFloor + room - keptTop;
  }
  const readingMode::Margins base{top, viewable.right + marginRight, bottom, viewable.left + marginLeft};
  const readingMode::Margins margins = readingMode::textMargins(mode, base, viewable, RickyReaderDog::kEdgeRow);
  if (!readingMode::setsVertically(mode)) return margins;
  return readingMode::fitColumns(
      margins, viewable, renderer.getScreenWidth(),
      renderer.getLineHeight(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression()));
}

void EpubReaderActivity::renderBook() {
  currentPageLinks.clear();
  if (!epub) return;
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();

  // Apply the reader's image-resampling choice before anything is drawn. The
  // setting can change from the reader menu, which does NOT reload the book, so
  // syncing only in loadBook() left the previous filter in force for the rest of
  // the session — the menu looked like it did nothing. The setter is a no-op
  // when the filter has not changed, so this costs a compare per render.
  ImageBlock::setBilinearScaling(SETTINGS.imageScaling == CrossPointSettings::IMAGE_SCALING_BILINEAR);

  const auto showPendingSyncSaveError = [this]() {
    if (pendingSyncSaveError) {
      pendingSyncSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    } else if (pendingSyncLaunchError) {
      pendingSyncLaunchError = false;
      GUI.drawPopup(renderer, tr(STR_SYNC_FAILED_MSG));
    }
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // Say why, with a code readers can quote; the details are in /rickyos-log.txt.
    switch (failedBuildError_) {
      case Section::BuildError::OutOfMemory:
        GUI.drawPopup(renderer, tr(STR_RICKY_INDEX_FAILED_MEMORY));
        return;
      case Section::BuildError::InvalidData:
        GUI.drawPopup(renderer, tr(STR_RICKY_INDEX_FAILED_DATA));
        return;
      case Section::BuildError::Io:
        GUI.drawPopup(renderer, tr(STR_RICKY_INDEX_FAILED_IO));
        return;
      default:
        break;
    }
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
  };

  if (failedBuildSpine_ == currentSpineIndex) {
    showBuildError();
    return;
  }
  failedBuildSpine_ = -1;
  failedBuildError_ = Section::BuildError::None;

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  const readingMode::Margins margins = textMargins();
  const int orientedMarginTop = margins.top;
  const int orientedMarginRight = margins.right;
  const int orientedMarginBottom = margins.bottom;
  const int orientedMarginLeft = margins.left;

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = effectiveRenderSpec(viewportWidth, viewportHeight);

  // A split TXT (Txt::Part): offsets are book-wide and name their part. Move there before the
  // section loads, so progress, bookmarks and jumps saved against the whole book (or before
  // the split) open only that part instead of laying the book out from its start.
  {
    std::optional<uint32_t> target = pendingOffsetJump;
    if (!target && !section && cachedSpineIndex == currentSpineIndex) target = cachedVisibleTextOffset;
    const int part = target ? epub->txtPartForVisibleOffset(*target) : -1;
    if (part >= 0 && part != currentSpineIndex) {
      if (cachedSpineIndex == currentSpineIndex) cachedSpineIndex = part;
      currentSpineIndex = part;
      nextPageNumber = 0;
      section.reset();
    }
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      LOG_ERR("ERS", "OOM: Section (%u bytes)", static_cast<unsigned>(sizeof(Section)));
      if (handleBuildFailure("section allocation", Section::BuildError::OutOfMemory)) return;
      showBuildError();
      return;
    }

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
    ++sectionGeneration_;
#endif
    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (Txt::isTxtOrMd(epub->getPath()) && !pendingAnchor.empty()) {
      uint32_t visibleOffset = 0;
      if (!Txt::resolveChapterPosition(epub->getPath(), epub->getCachePath(), pendingAnchor, visibleOffset)) {
        LOG_ERR("ERS", "Failed to resolve TXT chapter %s", pendingAnchor.c_str());
        showBuildError();
        return;
      }
      pendingOffsetJump = visibleOffset;
      pendingAnchor.clear();
    }
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        prepareChapterBuild();
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          loan.end();
          if (handleBuildFailure("full build", section->buildError())) return;
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Resuming a partial chapter re-parses it from the top: seconds in a long chapter.
        // When the saved page is already cached, show it now; updateChapterBuild() extends
        // the chapter in the background while the reader is on the page.
        constexpr int kOpenMargin = 0;
        if (section->isPartial() &&
            (offsetJump.has_value() ? section->getPageForVisibleTextOffset(*offsetJump).has_value()
             : anchorJump           ? section->getPageForAnchor(pendingAnchor).has_value()
                                    : target + kOpenMargin < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          // Section (re)builds are the heap-hungriest path (per-word
          // allocations for the whole section). Under TTF heap pressure, shed
          // every rebuildable font cache first — dropped glyphs re-fault on
          // demand after the build. Skipped for cpfont/builtin reading: the
          // release drops the persistent advance table and mini tables, which
          // would force SD metric re-reads on every fresh chapter for no gain.
          // With PSRAM the TTF arenas are not internal heap: dropping them freed nothing
          // the build needs and re-rasterized every glyph of the next page (device: 1.7-2.5 s
          // after each chapter or settings change). Shed them there only when internal
          // memory is actually short, as Section's own layout reclaim does.
#if defined(BOARD_HAS_PSRAM)
          const bool shedTtf = ESP.getFreeHeap() < 48 * 1024 || ESP.getMaxAllocHeap() < 16 * 1024;
#else
          constexpr bool shedTtf = true;
#endif
          if (shedTtf && !renderer.getTtfFonts().empty()) {
            if (auto* fcm = renderer.getFontCacheManager()) {
              fcm->releaseSdFontCaches();
            }
          }
          LOG_DBG("ERS", "Heap before section build: %u (max block %u)", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
          const unsigned long buildStartMs = millis();
          bool started;
          {
            prepareChapterBuild();
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            if (handleBuildFailure("start", section->buildError())) return;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              if (handleBuildFailure("foreground build", section->buildError())) return;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        legacyProgressPending = false;
        clearDeferredReposition();
      }
    }
    if (legacyProgressPending) {
      loadProtectionError = "txt progress migration failed";
      handleLoadFailure();
      return;
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
    if (stylesDisabledForSession_) {
      LOG_INF("ERS", "Basic layout ready: spine=%d page=%d (free=%u, min=%u, maxAlloc=%u)", currentSpineIndex,
              section->currentPage, static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding()) prepareChapterBuild();
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      if (handleBuildFailure("partial start", section->buildError())) return;
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        if (handleBuildFailure("extension build", section->buildError())) return;
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        if (handleBuildFailure("extension build", section->buildError())) return;
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  // Serialize SD access in this render path against the main task's SD writes
  // (progress, bookmarks, background build) so they cannot interleave mid-FAT-op.

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    reading_excerpt::remember(epub->getPath(), *p);  // for the standby card; RAM only
    renderedSpineIndex_ = currentSpineIndex;
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLinks = std::move(p->links);
    currentPageLinkMarginLeft = orientedMarginLeft;
    currentPageLinkMarginTop = orientedMarginTop;

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
#ifdef RICKYOS_PRODUCT
    LOG_INF("ERS", "Rendered page in %dms", millis() - start);
#else
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
#endif
    lastRenderCompleteMs = millis();
    markPageRendered();
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, dictionaryMessageVertical ? tr(STR_VERTICAL_SELECT_UNAVAILABLE) : tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    pushOverlayRefresh();
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  } else if (pendingSyncLaunchError) {
    pendingSyncLaunchError = false;
    GUI.drawPopup(renderer, tr(STR_SYNC_FAILED_MSG));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  if (legacyProgressPending) return false;
  int progressPercent = 0;
  if (epub->getBookSize() > 0 && pageCount > 0) {
    const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
    progressPercent =
        clampPercent(static_cast<int>(epub->calculateProgress(spineIndex, chapterProgress) * 100.0f + 0.5f));
  }

  std::optional<uint32_t> offset;
  if (footnoteDepth > 0 && savedPositions[0].spineIndex == spineIndex && savedPositions[0].pageNumber == currentPage) {
    offset = savedPositions[0].visibleTextOffset;
  } else if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  } else if (!section && spineIndex == currentSpineIndex && cachedSpineIndex == currentSpineIndex) {
    offset = cachedVisibleTextOffset;  // remembered before the section went away
  }
  uint8_t chapterProgress = getStatsChapterProgressPercent(currentPage, pageCount);
  const std::string chapterTitle = getStatsChapterTitle(*epub, spineIndex, offset, &chapterProgress);
  READING_STATS.updateProgress(static_cast<uint8_t>(progressPercent), progressPercent >= 100, chapterTitle,
                               chapterProgress);
  const bool saved = EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
  if (saved && markdownProgressPending_) {
    const std::string checkpoint = epub->getCachePath() + "/md-resume-v1.bin";
    if (Storage.remove(checkpoint.c_str())) markdownProgressPending_ = false;
  }
  if (saved && clearInitialProgressAfterSave_ && WeReadStore::clearInitialProgress(wereadBookId_)) {
    clearInitialProgressAfterSave_ = false;
  }
  return saved;
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

// A page that renders on the native 16-level path: anti-aliased text with no images or
// only JPEG ones (they decode straight into the frame). Must match use16LevelText.
bool EpubReaderActivity::nativeGray16Page(const Page& page) const {
#if FREEINK_DEVICE_METALIO_PLUS
  // Native motion needs the 4-bit transaction even when the user wants binary
  // text. readerTextWeightCurve() still thresholds glyphs with AA disabled;
  // enabling motion or a background must not change that typography preference.
  return !renderer.isInverted() && renderer.getGrayscaleLevels() == 16 &&
         (SETTINGS.textAntiAliasing || (SETTINGS.nativePageTurn > 0 && SETTINGS.nativePageTurn <= 3) ||
          page.hasImages() || page.hasPanelFill() || ReadingBackground::active());
#else
  return SETTINGS.textAntiAliasing && !renderer.isInverted() && renderer.getGrayscaleLevels() == 16 &&
         page.imagesAreAllJpeg();
#endif
}

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
bool EpubReaderActivity::pageCacheEligible() const {
  // No !section->isBuilding() here, deliberately. The page cache renders ONE page --
  // the current one or the next -- and Section::loadPage() is written to serve exactly
  // that while a build is in flight: it reads from the in-RAM build LUT, and returns
  // null for a page the build has not laid out yet. The caller already treats that null
  // as a clean skip. Gating the whole feature on the chapter being finished coupled two
  // independent things, and on this board the chapter build routinely does not finish:
  // the CSS parse needs 65,536 B and the device has ~48 KB free while reading, so the
  // build stalls (the log stops at "Page 8 processed" and never reports completion),
  // isBuilding() stays true for the rest of the session, and the cache then never
  // engages at all -- every turn is a full render instead of the cached one.
  return !pageCacheFailed_ && section && renderer.hasFrameBuffer() && !activityManager.isSwitchPending() &&
         overlay == Overlay::None && fontPromptState == FontPromptState::Idle && !renderer.isInverted() &&
         !renderer.supportsStripGrayscale() &&
#if FREEINK_DEVICE_METALIO_PLUS
         renderer.getGrayscaleLevels() == 16 &&
         (SETTINGS.textAntiAliasing || (SETTINGS.nativePageTurn > 0 && SETTINGS.nativePageTurn <= 3));
#else
         SETTINGS.textAntiAliasing && renderer.supportsTextOnlyCombinedBase();
#endif
}

ReaderPageCacheKey EpubReaderActivity::pageCacheKey(const int page, const int top, const int right, const int bottom,
                                                    const int left) const {
  return {
      .spec = effectiveRenderSpec(renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom),
      .sectionGeneration = sectionGeneration_,
      .renderEpoch = renderEpoch_,
      .spine = currentSpineIndex,
      .page = page,
      .top = top,
      .right = right,
      .bottom = bottom,
      .left = left,
      .orientation = SETTINGS.orientation,
      .fakeBold = SETTINGS.fakeBold,
      .textWeight = readerTextWeight(),
      .antiAliasing = SETTINGS.textAntiAliasing != 0,
      .inverted = renderer.isInverted(),
      .guideLine = SETTINGS.readingGuideLineEnabled != 0,
      .guideStyle = SETTINGS.readingGuideLineStyle,
      .guideOffset = SETTINGS.readingGuideLineOffset};
}

// A 16-level page builds in ~270 ms, so start soon after the turn: at 400 ms readers
// turning every second never found the next page ready. Input still cancels the build.
uint32_t EpubReaderActivity::idleRenderDelayMs() const { return pageCacheFailed_ ? 0 : 120; }

void EpubReaderActivity::freePageCache() {
  for (int slot = 0; slot < kPageCacheSlots; ++slot) {
    pageCacheBase_[slot].reset();
    pageCacheLsb_[slot].reset();
    pageCacheMsb_[slot].reset();
    pageCacheStash_[slot].reset();
    pageCache16_[slot].reset();
    pageCacheIs16_[slot] = false;
    pageCache_[slot].state = ReaderPageCache::State::Empty;
  }
}

#if FREEINK_DEVICE_METALIO_PLUS
// After the page cache: warm the TTF reader face's common characters, eight at a time
// while input stays quiet. A first-seen glyph costs ~4 ms to rasterize from the card, and
// page turns are where it was paid. Interrupted warm-up resumes on the next idle pass.
void EpubReaderActivity::renderIdleWarmup(const uint32_t generation) {
  if (bookFonts_ && bookFonts_->anyLoaded()) return;
#if RICKYOS_PLUS_READER_TIMING
  const auto warmStart = millis();
  unsigned batches = 0;
#endif
  while (!activityManager.idleRenderCancelled(generation) && sdFontSystem.prerasterizeCommon(8)) {
#if RICKYOS_PLUS_READER_TIMING
    ++batches;
#endif
  }
#if RICKYOS_PLUS_READER_TIMING
  if (batches) LOG_INF("ReaderCache", "ttf warm-up batches=%u in %lums", batches, millis() - warmStart);
#endif
}
#endif

void EpubReaderActivity::renderIdle(const uint32_t generation) {
  const auto cancelled = [&] { return activityManager.idleRenderCancelled(generation); };
#if RICKYOS_PLUS_READER_TIMING && FREEINK_DEVICE_METALIO_PLUS
  LOG_INF("ReaderCache", "idle eligible=%d cancelled=%d aa=%d failed=%d spine=%d page=%d epoch=%lu",
          pageCacheEligible(), cancelled(), SETTINGS.textAntiAliasing, pageCacheFailed_, currentSpineIndex,
          section ? section->currentPage : -1, static_cast<unsigned long>(renderEpoch_));
#endif
  // Every exit below names the condition it took. Without that, "the cache never hits"
  // is indistinguishable from "the build was cancelled", "the layout moved since the
  // last render" and "the next page has images" -- and each of those needs a different
  // fix. The lines are cheap: this hook runs at most once between two renders.
  if (cancelled()) {
    LOG_DBG("ERS", "Page cache: skip, cancelled before start");
    return;
  }
  if (!pageCacheEligible()) {
    // Name every input to pageCacheEligible(), because "not eligible" is ten conditions
    // at once and only one of them is the one that fires. The display-capability pair at
    // the end is the reason this needs saying out loud: on readpico the panel is driven
    // row by row from an LCD_CAM path, so supportsStripGrayscale() and
    // supportsTextOnlyCombinedBase() can legitimately differ from the SSD1677 boards the
    // gate was written against.
    LOG_DBG("ERS",
            "Page cache: skip, not eligible (failed=%d section=%d building=%d fb=%d switch=%d ovl=%d prompt=%d "
            "inv=%d aa=%d strip=%d combined=%d)",
            static_cast<int>(pageCacheFailed_), static_cast<int>(section != nullptr),
            static_cast<int>(section != nullptr && section->isBuilding()), static_cast<int>(renderer.hasFrameBuffer()),
            static_cast<int>(activityManager.isSwitchPending()), static_cast<int>(overlay != Overlay::None),
            static_cast<int>(fontPromptState != FontPromptState::Idle), static_cast<int>(renderer.isInverted()),
            static_cast<int>(SETTINGS.textAntiAliasing), static_cast<int>(renderer.supportsStripGrayscale()),
            static_cast<int>(renderer.supportsTextOnlyCombinedBase()));
    return;
  }
  const auto& layout = renderedPageKey_;
  // A main-loop reflow/jump may have replaced the section since the last render.
  if (layout != pageCacheKey(section->currentPage, layout.top, layout.right, layout.bottom, layout.left)) {
#if RICKYOS_PLUS_READER_TIMING && FREEINK_DEVICE_METALIO_PLUS
    LOG_INF("ReaderCache", "skip layout moved");
#endif
    LOG_DBG("ERS", "Page cache: skip, layout moved since the last render");
    return;
  }

  // Slot 0 covers the page a forward turn lands on, slot 1 the page a backward turn
  // lands on. Both are worth building: a reader turns both ways, and one slot could only
  // ever cover +1, which made every backward turn a guaranteed miss. A reading pace
  // leaves room for both (~700 ms each); an unusually fast test pace cuts the second
  // short, which is what the cancelled() check between them is for.
  // A build cut short by input stays retryable: the render task re-arms the idle pass
  // once input is quiet, and a Skipped slot would otherwise never be built again.
  const auto build = [&](const int slot, const ReaderPageCacheKey& key) {
    [[maybe_unused]] const auto buildStart = millis();
    const bool built = buildPageCacheSlot(slot, key, generation);
#if RICKYOS_PLUS_READER_TIMING && FREEINK_DEVICE_METALIO_PLUS
    if (!built) {
      LOG_INF("ReaderCache", "build slot=%d page=%d failed cancelled=%d state=%d after=%lums", slot, key.page,
              cancelled(), static_cast<int>(pageCache_[slot].state), millis() - buildStart);
    }
#endif
    if (!built && cancelled() && pageCache_[slot].key == key &&
        pageCache_[slot].state == ReaderPageCache::State::Skipped) {
      pageCache_[slot].state = ReaderPageCache::State::Empty;
    }
  };
  auto forward = layout;
  ++forward.page;
  if (forward.page < static_cast<int>(section->pageCount)) {
    activityManager.setTouchTolerantIdle(true);
    build(0, forward);
    activityManager.setTouchTolerantIdle(false);
  }
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  else {
    // At a chapter boundary, the next chapter is the forward page. Prepare it
    // before spending the idle window rendering the previous page.
    prefetchNextChapter(layout, generation);
  }
#endif
  if (cancelled() || pageCacheFailed_) return;
  auto backward = layout;
  --backward.page;
  if (backward.page >= 0) build(1, backward);
}

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
// On a chapter's last page the forward slot is free: build the next chapter and render
// its first page into it. Comics put one picture in each chapter, so without this every
// turn decoded a JPEG in the foreground (~3.5 s); now it happens while the page is read.
// Only small chapters: the build itself cannot be interrupted, so a long text chapter
// would hold up the next input. Its section file is kept, so the turn loads it quickly.
void EpubReaderActivity::prefetchNextChapter(const ReaderPageCacheKey& layout, const uint32_t generation) {
  constexpr size_t kMaxPrefetchChapterBytes = 24 * 1024;
  const int next = currentSpineIndex + 1;
  if (!section || section->isPartial() || section->isBuilding() || next >= epub->getSpineItemsCount()) return;
  const size_t bytes = epub->getCumulativeSpineItemSize(next) - epub->getCumulativeSpineItemSize(currentSpineIndex);
  if (bytes > kMaxPrefetchChapterBytes) return;
  if (ESP.getFreeHeap() < BACKGROUND_BUILD_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < BACKGROUND_BUILD_MIN_MAX_ALLOC) {
    LOG_DBG("ERS", "Next chapter prefetch: skip, low heap");
    return;
  }
  auto key = layout;
  key.spine = next;
  key.page = 0;
  key.sectionGeneration = sectionGeneration_ + 1;  // what the turn's new Section will carry
  if (pageCache_[0].attempted(key)) return;
  const auto started = millis();
  auto nextSection = makeUniqueNoThrow<Section>(epub, next, renderer);
  if (!nextSection) return;
  if (!nextSection->loadSectionFile(key.spec) || nextSection->isPartial()) {
    if (activityManager.idleRenderCancelled(generation)) return;
    if (!nextSection->createSectionFile(key.spec)) {
      LOG_DBG("ERS", "Next chapter prefetch: build failed (spine %d)", next);
      return;
    }
  }
  if (nextSection->pageCount == 0 || activityManager.idleRenderCancelled(generation)) return;
  const bool ready = buildPageCacheSlot(0, key, generation, nextSection.get());
  // Unlike the same-chapter path this call does not go through renderIdle's
  // build wrapper. Keep an interrupted image retryable here as well.
  if (!ready && activityManager.idleRenderCancelled(generation) && pageCache_[0].key == key &&
      pageCache_[0].state == ReaderPageCache::State::Skipped) {
    pageCache_[0].state = ReaderPageCache::State::Empty;
  }
  LOG_DBG("ERS", "Next chapter prefetch: spine %d ready=%d in %lums", next, ready, millis() - started);
}
#endif

// Build one page into one slot. Returns true when the slot ends up holding a finished
// page; every other exit leaves it Skipped and logs why. Runs only from renderIdle(),
// on the render task while holding the render lock. Cancellation must be bounded.
bool EpubReaderActivity::buildPageCacheSlot(const int slot, const ReaderPageCacheKey& key, const uint32_t generation,
                                            Section* source) {
  const auto cancelled = [&] { return activityManager.idleRenderCancelled(generation); };
  if (pageCache_[slot].attempted(key)) {
    LOG_DBG("ERS", "Page cache: slot %d skip, page %d already attempted (state=%d)", slot, key.page,
            static_cast<int>(pageCache_[slot].state));
    return false;
  }
  pageCache_[slot].key = key;
  pageCache_[slot].state = ReaderPageCache::State::Skipped;

  auto page = (source ? source : section.get())->loadPage(key.page);
  if (!page) {
    LOG_ERR("ERS", "Page cache: slot %d, page %d failed to load", slot, key.page);
    return false;
  }
  if (cancelled()) {
    LOG_DBG("ERS", "Page cache: slot %d, page %d cancelled after the load", slot, key.page);
    return false;
  }
  const bool native16 = nativeGray16Page(*page);
  if (page->hasImages() && !native16) {
    if (slot != 0 || !page->hasImagesNeedingDecode()) return false;
    const size_t bytes = renderer.getBufferSize();
    if (!bytes) return false;
    if (!pageCacheStash_[slot]) {
      if (!memory::psramHasHeadroom(bytes, bytes, 16 * 1024)) return false;
      pageCacheStash_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      if (!pageCacheStash_[slot]) return false;  // Optional work must not disable the text cache.
    }
    if (cancelled()) return false;
    auto* live = renderer.getFrameBuffer();
    memcpy(pageCacheStash_[slot].get(), live, bytes);
    const auto mode = renderer.getRenderMode();
    const ScopedCleanup restore{[&] {
      renderer.setRenderMode(mode);
      memcpy(live, pageCacheStash_[slot].get(), bytes);
      ImageBlock::releaseRenderCache();
    }};
    uint32_t idleGeneration = generation;
    const CancelCheck cancellation{&idleGeneration, [](void* context) {
                                     return activityManager.idleRenderCancelled(*static_cast<uint32_t*>(context));
                                   }};
    const auto started = millis();
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool ready = page->warmImages(renderer, key.left, key.top, cancellation);
    LOG_DBG("ERS", "Image prewarm: page=%d ready=%d cancelled=%d time=%lums", key.page, ready,
            cancellation.isCancelled(), millis() - started);
    return false;  // Only .pxc is warmed; foreground still renders the grayscale image.
  }

  const size_t bytes = renderer.getBufferSize();
  if (bytes == 0) return false;
  if (!pageCacheBase_[slot]) {
    constexpr size_t kContiguousReserve = 16 * 1024;
    // Reserve the slots still to allocate; the forward slot is already charged for slot 1.
    const size_t planes = native16 ? 2 : 4;  // 16-level pages need a B/W proxy and a stash, no gray bitplanes.
    const size_t slotBytes = planes * bytes + (native16 ? renderer.grayscale16FrameBytes() : 0);
    if (memory::psramHasHeadroom(static_cast<size_t>(kPageCacheSlots - slot) * slotBytes, bytes, kContiguousReserve)) {
      pageCacheBase_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      if (!native16) {
        pageCacheLsb_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
        pageCacheMsb_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
      }
      if (!pageCacheStash_[slot]) pageCacheStash_[slot] = memory::makePsramByteBufferUninitializedNoThrow(bytes);
    }
    if (!pageCacheBase_[slot] || (!native16 && (!pageCacheLsb_[slot] || !pageCacheMsb_[slot])) ||
        !pageCacheStash_[slot]) {
      freePageCache();
      pageCacheFailed_ = true;
      LOG_ERR("ERS", "Page cache off: PSRAM allocation of %u B failed", static_cast<unsigned>(4 * bytes));
      return false;
    }
  }
  if (native16 && !pageCache16_[slot]) {
    const size_t frameBytes = renderer.grayscale16FrameBytes();
    if (memory::psramHasHeadroom(frameBytes, frameBytes, 64 * 1024)) {
      pageCache16_[slot] = memory::makePsramByteBufferUninitializedNoThrow(frameBytes);
    }
    if (!pageCache16_[slot]) {
      LOG_ERR("ERS", "Page cache: slot %d, no PSRAM for a 16-level page (%u B)", slot,
              static_cast<unsigned>(frameBytes));
      return false;
    }
  }
  if (cancelled()) return false;

  uint8_t* const live = renderer.getFrameBuffer();
  auto* fcm = renderer.getFontCacheManager();
  const auto started = millis();
  if (fcm) fcm->consumeMissingChineseCodepoint();
  memcpy(pageCacheStash_[slot].get(), live, bytes);
  struct RestoreFrame {
    GfxRenderer& renderer;
    uint8_t* live;
    const uint8_t* stash;
    size_t bytes;
    GfxRenderer::RenderMode mode;
    ~RestoreFrame() {
      renderer.setRenderMode(mode);
      memcpy(live, stash, bytes);
      if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
    }
  } restore{renderer, live, pageCacheStash_[slot].get(), bytes, renderer.getRenderMode()};
  if (fcm) {
    fcm->clearCache();
    fcm->resetStats();
  }

  const bool vertical = key.spec.verticalText;
  const readingMode::Box textBox = readingMode::textBox(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                                        {key.top, key.right, key.bottom, key.left});
  const int pitch = renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression);
  uint32_t idleGeneration = generation;
  const CancelCheck cancellation{&idleGeneration, [](void* context) {
                                   return activityManager.idleRenderCancelled(*static_cast<uint32_t*>(context));
                                 }};
  const auto renderCachedElement = [&](PageElement& element) {
    if (native16 && !vertical && element.getTag() == TAG_PageImage) {
      auto& image = static_cast<PageImage&>(element);
      ImageRenderError error = ImageRenderError::None;
      image.getImageBlock().render(renderer, element.xPos + key.left, element.yPos + key.top,
                                   ImageBlock::PixelCachePolicy::Stream, &error, cancellation);
      // Ordinary failures retain the existing placeholder. Cancellation must
      // never publish a half-decoded image as a finished page-cache hit.
      return error != ImageRenderError::Cancelled;
    }
    if (vertical) {
      element.renderVertical(renderer, key.spec.fontId, textBox.right(), textBox.y, pitch);
    } else {
      element.render(renderer, key.spec.fontId, key.left, key.top);
    }
    return true;
  };

  // Load glyphs on demand: a whole-page prewarm cannot be interrupted. Check
  // between elements instead; an individual SD read/glyph render must finish.
  const auto renderPlane = [&](const GfxRenderer::RenderMode mode, uint8_t* target) {
    if (cancelled()) return false;
    renderer.setRenderMode(mode);
    // clearScreen() reaches the HAL on Read Pico; this is scratch only.
    memset(live, mode == GfxRenderer::BW ? 0xFF : 0x00, bytes);
    GfxRenderer::SyntheticBoldScope syntheticBold(renderer, key.fakeBold);
    GfxRenderer::TextWeightScope textWeight(renderer, readerTextWeightCurve(key.textWeight),
                                            readerTextWeightSpread(key.textWeight));
    if (!vertical) page->renderPanels(renderer, key.spec.fontId, key.left, key.top);
    for (const auto& element : page->elements) {
      if (cancelled()) return false;
      if (!renderCachedElement(*element)) return false;
    }
    if (vertical) readerChrome::drawColumnRules(renderer, *page, textBox, pitch);
    // Match foreground ordering: all text first, then the guide lines.
    for (const auto& element : page->elements) {
      if (cancelled()) return false;
      if (!key.guideLine || vertical || element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock()->isEmpty()) continue;
      const int ascender = renderer.getFontAscenderSize(key.spec.fontId);
      const int lineHeight =
          renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression) + line.getBlock()->getRubyShift(ascender);
      const int guideY =
          key.top + line.yPos +
          readingGuideLine::rowOffset(lineHeight, ascender, readingGuideLine::emFor(renderer, key.spec.fontId)) +
          key.guideOffset;
      if (readingGuideLine::fitsVertically(key.guideStyle, guideY, key.top, renderer.getScreenHeight() - key.bottom)) {
        readingGuideLine::draw(renderer, key.left, guideY, renderer.getScreenWidth() - key.right - 1, key.guideStyle);
      }
    }
    if (cancelled()) return false;
    memcpy(target, live, bytes);
    return !cancelled();
  };

  if (native16) {
    // One pass into the PSRAM 4bpp frame; the B/W proxy lands in the scratch framebuffer.
    pageCacheIs16_[slot] = false;
    if (!renderer.beginGrayscale16Offscreen(pageCache16_[slot].get())) return false;
    struct EndOffscreen {
      GfxRenderer& renderer;
      ~EndOffscreen() { renderer.endGrayscale16Offscreen(); }
    } endOffscreen{renderer};
#if FREEINK_DEVICE_METALIO_PLUS
    {
      // Ruled paper follows this page's text box and rows.
      ReadingBackground::TextBox box{key.left, key.top, renderer.getScreenWidth() - key.right,
                                     renderer.getScreenHeight() - key.bottom};
      if (!vertical) {
        box.pitch = renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression);
        box.rule = readingGuideLine::rowOffset(box.pitch, renderer.getFontAscenderSize(key.spec.fontId),
                                               readingGuideLine::emFor(renderer, key.spec.fontId));
      }
      ReadingBackground::paint(renderer, box);
    }
#endif
    {
      GfxRenderer::SyntheticBoldScope syntheticBold(renderer, key.fakeBold);
      GfxRenderer::TextWeightScope textWeight(renderer, readerTextWeightCurve(key.textWeight),
                                              readerTextWeightSpread(key.textWeight));
      if (!vertical) page->renderPanels(renderer, key.spec.fontId, key.left, key.top);
      for (const auto& element : page->elements) {
        if (cancelled()) return false;
        if (!renderCachedElement(*element)) return false;
      }
      if (vertical) readerChrome::drawColumnRules(renderer, *page, textBox, pitch);
    }
    // Rendered: keep it even if input arrived meanwhile. That input is usually the turn
    // to this very page; on a comic the element above was a ~2 s JPEG decode, and
    // throwing it away made the turn decode it all over again in the foreground.
    for (const auto& element : page->elements) {
      if (!key.guideLine || vertical || element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock()->isEmpty()) continue;
      const int ascender = renderer.getFontAscenderSize(key.spec.fontId);
      const int lineHeight =
          renderer.getLineHeight(key.spec.fontId, key.spec.lineCompression) + line.getBlock()->getRubyShift(ascender);
      const int guideY =
          key.top + line.yPos +
          readingGuideLine::rowOffset(lineHeight, ascender, readingGuideLine::emFor(renderer, key.spec.fontId)) +
          key.guideOffset;
      if (readingGuideLine::fitsVertically(key.guideStyle, guideY, key.top, renderer.getScreenHeight() - key.bottom)) {
        readingGuideLine::draw(renderer, key.left, guideY, renderer.getScreenWidth() - key.right - 1, key.guideStyle);
      }
    }
    memcpy(pageCacheBase_[slot].get(), live, bytes);
    pageCacheIs16_[slot] = true;
  } else if (!renderPlane(GfxRenderer::BW, pageCacheBase_[slot].get()) ||
             !renderPlane(GfxRenderer::GRAYSCALE_LSB, pageCacheLsb_[slot].get()) ||
             !renderPlane(GfxRenderer::GRAYSCALE_MSB, pageCacheMsb_[slot].get()) || cancelled()) {
    return false;
  } else {
    pageCacheIs16_[slot] = false;
  }
  pageCacheMissingCodepoint_[slot] = fcm ? fcm->consumeMissingChineseCodepoint() : 0;
  pageCache_[slot].state = ReaderPageCache::State::Ready;
#if RICKYOS_PLUS_READER_TIMING && FREEINK_DEVICE_METALIO_PLUS
  LOG_INF("ReaderCache", "ready slot=%d spine=%d page=%d epoch=%lu ms=%lu", slot, key.spine, key.page,
          static_cast<unsigned long>(key.renderEpoch), millis() - started);
#endif
  LOG_DBG("ERS", "Page cache: slot %d, page %d ready in %lums", slot, key.page, millis() - started);
  return true;
}
#endif

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  [[maybe_unused]] const auto nativeTurnRequest = pendingNativeTurn_.exchange(0);
  [[maybe_unused]] const int nativeTurnDirection = nativePageTurn::direction(nativeTurnRequest);
  [[maybe_unused]] const bool previousNativeText = std::exchange(previousNativeText_, false);
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();
  const bool vertical = readingMode::setsVertically(readingMode::modeFor(SETTINGS.readingMode));
  const readingMode::Box textBox =
      readingMode::textBox(renderer.getScreenWidth(), renderer.getScreenHeight(),
                           {orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft});
  const int pitch = renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression());
  const auto renderPage = [&] {
    GfxRenderer::SyntheticBoldScope syntheticBold(renderer, SETTINGS.fakeBold);
    GfxRenderer::TextWeightScope textWeight(renderer, readerTextWeightCurve(readerTextWeight()),
                                            readerTextWeightSpread(readerTextWeight()));
    if (vertical) {
      page->renderVertical(renderer, fontId, textBox.right(), textBox.y, pitch);
      readerChrome::drawColumnRules(renderer, *page, textBox, pitch);
      return;
    }
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  };

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  const bool pageHasImages = page->hasImages();
  // JPEGs on a 16-level page decode straight into the native frame, so the dithered
  // .pxc cache this predecode builds would be thrown away (it doubled the decode time).
  const bool nativeGrayImages = pageHasImages && nativeGray16Page(*page);
  const bool pageHasImagesNeedingDecode = pageHasImages && !nativeGrayImages && page->hasImagesNeedingDecode();
  if (pageHasImagesNeedingDecode) {
    // Lend the fixed framebuffer to the large JPEG/PNG decoder while it streams
    // the cold image into .pxc. Formal rendering happens after the loan ends.
    fcm->clearCache();
    fcm->releaseSdFontCaches();
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      page->extractImagesNeedingDecode();
      page->cacheImagesNeedingDecode(renderer, orientedMarginLeft, orientedMarginTop);
    }
    ImageBlock::releaseRenderCache();
    renderer.clearScreen();
  }
  if (pageHasImages) {
    LOG_DBG("ERS", "Image predecode=%lums cold=%u", millis() - t0, static_cast<unsigned>(pageHasImagesNeedingDecode));
  }

  fcm->consumeMissingChineseCodepoint();
  struct RawFontCacheGuard {
    FontCacheManager* manager = nullptr;
    ~RawFontCacheGuard() {
      if (manager) manager->clearCache();
    }
  } rawFontCacheGuard;
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  const auto key = pageCacheKey(section ? section->currentPage : -1, orientedMarginTop, orientedMarginRight,
                                orientedMarginBottom, orientedMarginLeft);
  // Either slot may hold this page: slot 0 was built for it as a forward turn, slot 1
  // as a backward one. Whichever matched is recorded, because the three plane copies
  // below must read that slot and not assume one of them.
  bool pageCacheHit = false;
  if (pageCacheEligible()) {
    for (int slot = 0; slot < kPageCacheSlots; ++slot) {
      if (pageCache_[slot].ready(key)) {
        pageCacheLiveSlot_ = slot;
        pageCacheHit = true;
        break;
      }
    }
  }
  ++renderEpoch_;
  renderedPageKey_ = key;
  renderedPageKey_.renderEpoch = renderEpoch_;
#else
  [[maybe_unused]] constexpr bool pageCacheHit = false;
#endif
  std::optional<FontCacheManager::PrewarmScope> prewarmScope;
  // A hit copies finished pixels; it does not need reader glyphs or a scan pass.
  if (!pageCacheHit && fcm->needsPrewarmScan(fontId)) {
    prewarmScope.emplace(*fcm);
    // Scan pass records the page text only (status bar glyphs are flash-resident
    // UI fonts and would otherwise shadow the reader font in the prewarm).
    renderPage();
    prewarmScope->endScanAndPrewarm();
  } else if (!pageCacheHit) {
    fcm->clearCache();
    fcm->resetStats();
    rawFontCacheGuard.manager = fcm;
  }
  const auto tPrewarm = millis();

  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  // Night mode renders crisp B/W; the SDK disables every grayscale display path.
  const bool grayscaleEnabled = !renderer.isInverted();
  const bool needsTextGrayscale = grayscaleEnabled && SETTINGS.textAntiAliasing;
  // Images write the BW buffer directly; keep them, backgrounds and faux bold on the existing path.
  // RickyOS: every text face (2-bit downloaded fonts too) and synthetic bold draw straight
  // into the 16-level frame, and JPEG illustrations decode into it, so anti-aliased pages
  // take the single-render path.
  const bool use16LevelText = nativeGray16Page(*page);
#if FREEINK_DEVICE_METALIO_PLUS
  if (SETTINGS.nativePageTurn && nativeTurnDirection) {
    LOG_INF("NativeTurn", "reader style=%u aa=%u native=%d dir=%d previous=%d images=%d inverted=%d",
            static_cast<unsigned>(SETTINGS.nativePageTurn), static_cast<unsigned>(SETTINGS.textAntiAliasing),
            use16LevelText, nativeTurnDirection, previousNativeText, pageHasImages, renderer.isInverted());
  }
#endif
  const bool needsAnyGrayscale = grayscaleEnabled && (SETTINGS.textAntiAliasing || pageHasImages);
  const bool absoluteImageGrayscale = grayscaleEnabled && pageHasImages && !gpio.deviceIsX3() &&
                                      display.getController() == HalDisplay::Controller::UC8279 &&
                                      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  const auto grayscale = renderer.grayscaleCapabilities(absoluteImageGrayscale ? HalDisplay::GrayscaleMode::Absolute
                                                                               : HalDisplay::GrayscaleMode::Overlay);
  const bool tiledGrayscale = needsAnyGrayscale && grayscale.stripUploads;
  // Combined text AA: defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase =
#if FREEINK_DEVICE_READPICO
      // Read Pico has no strip uploads (whole-plane grayscale) but its panel CAN join
      // the base and the grey planes into a single waveform, so it must not be gated
      // behind tiledGrayscale. This is what removes the extra full panel refresh per
      // anti-aliased page: the base is stashed and displayGrayBuffer() presents both.
      //
      // Image pages are included here, unlike the shared branch below. Its
      // !pageHasImages exists because a strip-upload driver has to re-send the
      // affected strip to build the base; this whole-plane path stashes the entire
      // framebuffer, illustration and all, so displayGrayBuffer() composes the page
      // from it either way and the panel is still driven exactly once.
      needsAnyGrayscale && renderer.supportsTextOnlyCombinedBase();
#else
      tiledGrayscale && !pageHasImages && renderer.supportsTextOnlyCombinedBase();
#endif
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  const auto drawGuideLines = [&] {
    // Columns carry their own 界行.
    if (!SETTINGS.readingGuideLineEnabled || vertical) return;
    const int x1 = orientedMarginLeft;
    const int x2 = renderer.getScreenWidth() - orientedMarginRight - 1;
    const int contentBottom = renderer.getScreenHeight() - orientedMarginBottom;
    const int baseLineHeight = renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression());
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int em = readingGuideLine::emFor(renderer, fontId);
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock()->isEmpty()) continue;
      const int lineHeight = baseLineHeight + line.getBlock()->getRubyShift(ascender);
      const int guideY = orientedMarginTop + line.yPos + readingGuideLine::rowOffset(lineHeight, ascender, em) +
                         SETTINGS.readingGuideLineOffset;
      if (readingGuideLine::fitsVertically(SETTINGS.readingGuideLineStyle, guideY, orientedMarginTop, contentBottom)) {
        readingGuideLine::draw(renderer, x1, guideY, x2, SETTINGS.readingGuideLineStyle);
      }
    }
  };
  const auto renderPageWithGuideLines = [&] {
    renderPage();
    drawGuideLines();
  };
  auto renderGrayscalePass = [&]() {
    if (absoluteImageGrayscale || needsTextGrayscale) {
      renderPageWithGuideLines();
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    if (absoluteImageGrayscale) renderStatusBar();
  };

  unsigned long cacheBaseMs = 0;
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  if (use16LevelText) {
    // The 16-level pass below clears the frame and renders (or copies from the cache)
    // the page, writing the B/W proxy as it goes, so a B/W render here is thrown away.
  } else if (pageCacheHit) {
    const auto tBase = millis();
    // The cache is the finished page, cleared margins and all, so no clearScreen
    // is needed on this path.
    memcpy(renderer.getFrameBuffer(), pageCacheBase_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
    cacheBaseMs = millis() - tBase;
  } else
#endif
  {
    renderPageWithGuideLines();
  }
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  const uint32_t missingCodepoint =
      pageCacheHit ? pageCacheMissingCodepoint_[pageCacheLiveSlot_] : fcm->consumeMissingChineseCodepoint();
#else
  const uint32_t missingCodepoint = fcm->consumeMissingChineseCodepoint();
#endif
  if (missingCodepoint != 0 && !FontDownloadActivity::wasChineseFontPromptShownThisBoot()) {
    uint32_t expected = 0;
    pendingMissingChineseCodepoint_.compare_exchange_strong(expected, missingCodepoint, std::memory_order_relaxed);
  }
#if FREEINK_DEVICE_METALIO_PLUS
  // beginGrayscale16() clears both frames. Drawing the B/W chrome here was
  // discarded immediately; the native pass (and its failure fallback) draws
  // the same chrome after the page, including its B/W proxy for popups.
  if (!use16LevelText) renderStatusBar();
#else
  renderStatusBar();
#endif
  const auto tBwRender = millis();

  if (absoluteImageGrayscale) {
    const auto baseMode = cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
    if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, baseMode)) {
      LOG_ERR("ERS", "Could not start absolute image page; displaying B/W");
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
      return;
    }
    LOG_DBG("ERS", "UC8279 image page: absolute quality waveform");
    pagesUntilFullRefresh = 1;
  } else if (use16LevelText) {
    // 16 级文字：下面那一趟 commitGrayscale16() 自己把整帧推给面板，所以这里**不**推底图。
    // 多推一次会变成两次面板驱动（更闪、更慢），而 16 级用的本身就是整屏波形，残影照清。
    // / 16-level text: the single commitGrayscale16() below drives the panel with the whole
    // frame, so the base is deliberately NOT pushed here. Pushing it would drive the panel
    // twice (more flash, more time), and the 16-level waveform is a full-screen one anyway,
    // so ghosting is still cleared.
  } else if (combinedGrayscaleBase) {
    if (std::exchange(lastPageHadPanelFill_, false)) pagesUntilFullRefresh = 1;
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh, manualRefreshPending);
  } else if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    if (renderer.supportsContinuousImageReading()) {
      renderer.displayBuffer(ReaderUtils::consumeRefreshMode(pagesUntilFullRefresh),
                             DisplayRefreshContext::ImageReading);
    } else {
      renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH,
                             DisplayRefreshContext::ImageReading);
      pagesUntilFullRefresh = 1;
    }
  } else {
    if (std::exchange(lastPageHadPanelFill_, false)) pagesUntilFullRefresh = 1;
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  if (use16LevelText) {
    // Native begin clears the frame, so render the complete page again.
    const auto t16Begin = millis();
    if (renderer.beginGrayscale16()) {
      const auto t16Cleared = millis();
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
      if (pageCacheHit && pageCacheIs16_[pageCacheLiveSlot_] && pageCache16_[pageCacheLiveSlot_]) {
        memcpy(renderer.grayscale16Frame(), pageCache16_[pageCacheLiveSlot_].get(), renderer.grayscale16FrameBytes());
        memcpy(renderer.getFrameBuffer(), pageCacheBase_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
        LOG_DBG("ERS", "Page render: 16-level page-cache hit");
      } else
#endif
      {
#if FREEINK_DEVICE_METALIO_PLUS
        {
          // Ruled paper follows this page's text box and rows.
          ReadingBackground::TextBox box{orientedMarginLeft, orientedMarginTop,
                                         renderer.getScreenWidth() - orientedMarginRight,
                                         renderer.getScreenHeight() - orientedMarginBottom};
          if (!vertical) {
            box.pitch = renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression());
            box.rule = readingGuideLine::rowOffset(box.pitch, renderer.getFontAscenderSize(fontId),
                                                   readingGuideLine::emFor(renderer, fontId));
          }
          ReadingBackground::paint(renderer, box);
        }
#endif
        renderPageWithGuideLines();
      }
      const auto t16Page = millis();
      renderStatusBar();
      LOG_DBG("ERS", "16-level timing: before=%lums begin=%lums page=%lums status=%lums", t16Begin - t0,
              t16Cleared - t16Begin, t16Page - t16Cleared, millis() - t16Page);
#if RICKYOS_PLUS_READER_TIMING
      LOG_INF("ReaderTiming", "start=%lu spine=%d page=%d images=%d cached=%d prewarm=%lums before=%lums begin=%lums "
                              "paint=%lums status=%lums",
              static_cast<unsigned long>(t0), currentSpineIndex, section ? section->currentPage : -1,
              pageHasImages ? 1 : 0, pageCacheHit ? 1 : 0,
              tPrewarm - t0, t16Begin - tPrewarm, t16Cleared - t16Begin, t16Page - t16Cleared,
              millis() - t16Page);
#endif
#if RICKYOS_PAINT_PROFILE
      {
        // Device breakdown of a reader page (profiling builds only).
        auto& profile = GfxRenderer::paintProfile();
        LOG_INF("PERF",
                "reader page prewarm=%lums before=%lums page=%lums status=%lums cached=%d weight=%u bold=%u "
                "text=%lums/%lu fill=%lums image=%lums glyph=%lums/%lu",
                tPrewarm - t0, t16Begin - tPrewarm, t16Page - t16Cleared, millis() - t16Page, pageCacheHit ? 1 : 0,
                static_cast<unsigned>(SETTINGS.rickyTextWeight), static_cast<unsigned>(SETTINGS.fakeBold),
                profile.us[1] / 1000, profile.calls[1], profile.us[0] / 1000, profile.us[3] / 1000, profile.us[4] / 1000,
                profile.calls[4]);
        profile = {};
        fcm->logStats("page");
#if CROSSPOINT_VECTOR_FONTS
        auto& ttf = TtfEpdFont::counters();
        LOG_INF("PERF", "reader ttf rasterized=%lu in %lums releases=%lu", static_cast<unsigned long>(ttf.rasterized),
                static_cast<unsigned long>(ttf.rasterUs / 1000), static_cast<unsigned long>(ttf.releases));
        const auto sd = SdCardFontSystem::takeStreamStats();
        LOG_INF("PERF", "reader ttf sd reads=%lu bytes=%lu in %lums", static_cast<unsigned long>(sd.reads),
                static_cast<unsigned long>(sd.bytes), static_cast<unsigned long>(sd.us / 1000));
        ttf = {};
#endif
      }
#endif
      if (activityManager.isSwitchPending()) {
        renderer.cancelGrayscale16();
        return;
      }
#if !defined(SIMULATOR)
      // Ordinary turns use the reader table; Plus retains its black-body re-drive,
      // but holds unchanged chrome using the logical text-box hint below. The
      // page the refresh cycle marks as due still cleans the full frame with GC16.
      // (The simulator's HAL comes from an external package without this switch.)
      const auto refresh = ReaderUtils::consumeRefreshMode(pagesUntilFullRefresh);
      const bool panelFill = !vertical && page->hasPanelFill();
      const bool panelTurn = panelFill || lastPageHadPanelFill_;
      lastPageHadPanelFill_ = panelFill;
      // A panel page (and the page leaving one) is itself a full GC16 clean, so the
      // interval counts again from here; otherwise the due clean followed it early.
      if (panelTurn && refresh == HalDisplay::FAST_REFRESH) pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
#if FREEINK_DEVICE_METALIO_PLUS
      const bool readerBackground = ReadingBackground::active();
      // Profile 5 retains the paper erase; profile 1 keeps the accepted white-page
      // table. Scheduled/manual/panel cleans still use profile 2 in either case.
      display.setNextGray16Profile(refresh == HalDisplay::FAST_REFRESH && !panelTurn ? (readerBackground ? 5 : 1) : 2);
#else
      display.setNextGray16Profile(refresh == HalDisplay::FAST_REFRESH && !panelTurn ? 1 : 2);
#endif
      // Outside the device branches: the Plus has its own profile branch above.
      display.setReaderFastRefresh(SETTINGS.readerFastRefresh != 0);
#if FREEINK_DEVICE_METALIO_PLUS
      if (SETTINGS.nativePageTurn && nativeTurnDirection) {
        LOG_INF("NativeTurn", "gate fast=%d panel=%d adjacent=%d orientation=%d",
                refresh == HalDisplay::FAST_REFRESH, panelTurn,
                previousNativeSpine_ == currentSpineIndex &&
                    previousNativePage_ + nativeTurnDirection == section->currentPage,
                previousNativeOrientation_ == static_cast<uint8_t>(renderer.getOrientation()));
      }
#endif
      // A turn queued while the previous page was still driving (it lands right after
      // that drive ends) means the reader is ahead of the panel: skip the motion, which
      // only adds stagger scans, so a run of quick taps turns each page faster.
      const bool queuedTurn = lastNativeCommitEnd_ != 0 && lastPageTurnTime - lastNativeCommitEnd_ < kQueuedTurnMs;
      if (nativeTurnDirection != 0 && previousNativeText && !pageHasImages && !queuedTurn &&
          refresh == HalDisplay::FAST_REFRESH && !panelTurn &&
          previousNativeSpine_ == currentSpineIndex &&
          previousNativePage_ + nativeTurnDirection == section->currentPage &&
          previousNativeOrientation_ == static_cast<uint8_t>(renderer.getOrientation())) {
        display.setNextPageTurn(SETTINGS.nativePageTurn, static_cast<uint8_t>(renderer.getOrientation()),
                                (nativeTurnDirection > 0) != (SETTINGS.pageTurnDirection != 0),
                                nativePageTurn::originX(nativeTurnRequest), nativePageTurn::originY(nativeTurnRequest));
      }
#if FREEINK_DEVICE_METALIO_PLUS
      if (refresh == HalDisplay::FAST_REFRESH && !panelTurn) {
        renderer.setNextGray16ReaderRefresh(orientedMarginLeft, orientedMarginTop,
            renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
            renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
        // White-paper text only: background grays retain the gallery's erase
        // sequence, not the text-only convergence timing. Motion is independent.
        display.setNextGray16ReaderTextFinish(!pageHasImages && !readerBackground);
      }
#endif
#endif
      const auto tCommit = millis();
      if (renderer.commitGrayscale16()) {
        previousNativeText_ = !pageHasImages && !lastPageHadPanelFill_;
        previousNativeSpine_ = currentSpineIndex;
        previousNativePage_ = section->currentPage;
        previousNativeOrientation_ = static_cast<uint8_t>(renderer.getOrientation());
        const auto tEnd = millis();
        lastNativeCommitEnd_ = tEnd;
        LOG_DBG("ERS", "Page render: 16-level text total=%lums", tEnd - t0);
#if RICKYOS_PLUS_READER_TIMING
        LOG_INF("ReaderTiming", "commit=%lums total=%lums images=%d cached=%d", tEnd - tCommit, tEnd - t0,
                pageHasImages ? 1 : 0, pageCacheHit ? 1 : 0);
#endif
#if RICKYOS_PAINT_PROFILE
        LOG_INF("PERF", "reader commit=%lums total=%lums", tEnd - tCommit, tEnd - t0);
#endif
#if defined(SIMULATOR)
        pagesUntilFullRefresh = 1;
#endif
        return;
      }
      LOG_ERR("ERS", "16-level text commit failed; displaying B/W");
    } else {
      LOG_ERR("ERS", "Could not start the 16-level text pass; displaying B/W");
    }
    renderer.cancelGrayscale16();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    renderPageWithGuideLines();
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    pagesUntilFullRefresh = 1;
    return;
  } else if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;
    const auto abandonGrayscale = [&] {
      renderer.setRenderMode(GfxRenderer::BW);
      if (combinedGrayscaleBase)
        renderer.cancelGrayscale();
      else
        renderer.cleanupGrayscaleWithFrameBuffer();
    };

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return memory::hasAllocationHeadroom(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), planeBytes, planeBytes,
                                           PLANE_BUF_HEADROOM, PLANE_BUF_MAX_ALLOC_RESERVE);
    };
    const auto allocatePlane = [&] {
      if (planeBufFits()) {
        auto buffer = memory::makeInternalByteBufferNoThrow(planeBytes);
        if (buffer) return buffer;
      }
      if (memory::psramHasHeadroom(planeBytes, planeBytes, PLANE_BUF_MAX_ALLOC_RESERVE)) {
        auto buffer = memory::makePsramByteBufferNoThrow(planeBytes);
        if (buffer) return buffer;
      }
      return memory::ByteBuffer{};
    };
    auto lsbPlaneBuf = overlapRefresh ? allocatePlane() : memory::ByteBuffer{};
    auto msbPlaneBuf = lsbPlaneBuf ? allocatePlane() : memory::ByteBuffer{};

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      // Abort before expensive grayscale display if a push/pop is pending
      if (activityManager.isSwitchPending()) {
        abandonGrayscale();
        return;
      }

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          if (combinedGrayscaleBase && activityManager.isSwitchPending()) {
            abandonGrayscale();
          } else {
            renderer.cleanupGrayscaleWithFrameBuffer();
          }
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          if (activityManager.isSwitchPending()) {
            abandonGrayscale();
            return;
          }
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          if (activityManager.isSwitchPending()) {
            abandonGrayscale();
            return;
          }
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        if (combinedGrayscaleBase && activityManager.isSwitchPending()) {
          abandonGrayscale();
          return;
        }
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        if (absoluteImageGrayscale) renderer.setRenderMode(GfxRenderer::BW);
        return;
      }
      const auto tBwStore = millis();

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
      if (pageCacheHit) {
        // Both planes were rendered at idle; putting them in place is one copy
        // each instead of a full page render each.
        memcpy(renderer.getFrameBuffer(), pageCacheLsb_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
        renderer.copyGrayscaleLsbBuffers();
      } else
#endif
      {
        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderGrayscalePass();
        renderer.copyGrayscaleLsbBuffers();
      }
      const auto tGrayLsb = millis();

      // Abort early if a push/pop is pending (e.g. user opened menu)
      if (activityManager.isSwitchPending()) {
        renderer.setRenderMode(GfxRenderer::BW);
        // A combined base was only stashed, so it has to be flushed, not restored.
        if (combinedGrayscaleBase)
          renderer.cancelGrayscale();
        else
          renderer.restoreBwBuffer();
        return;
      }

#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
      if (pageCacheHit) {
        memcpy(renderer.getFrameBuffer(), pageCacheMsb_[pageCacheLiveSlot_].get(), renderer.getBufferSize());
        renderer.copyGrayscaleMsbBuffers();
      } else
#endif
      {
        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderGrayscalePass();
        renderer.copyGrayscaleMsbBuffers();
      }
      const auto tGrayMsb = millis();

      // Abort before the expensive grayscale display if a push/pop is pending
      if (activityManager.isSwitchPending()) {
        renderer.setRenderMode(GfxRenderer::BW);
        // A combined base was only stashed, so it has to be flushed, not restored.
        if (combinedGrayscaleBase)
          renderer.cancelGrayscale();
        else
          renderer.restoreBwBuffer();
        return;
      }
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      if (pageCacheHit) {
        LOG_DBG("ERS", "Page render: page-cache hit (base_copy=%lums)", cacheBaseMs);
      }
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  [[maybe_unused]] const auto perfStart = millis();
  [[maybe_unused]] unsigned long perfLookup = 0, perfChrome = 0;
  const int currentPage = section ? section->currentPage + 1 : 1;
  const auto mode = readingMode::modeFor(SETTINGS.readingMode);
  if (mode != readingMode::Mode::Original && mode != readingMode::Mode::Immersive) {
    [[maybe_unused]] const auto perfLookupStart = millis();
    readerChrome::PageInfo info;
    info.bookTitle = epub ? epub->getTitle().c_str() : nullptr;
    // A chapter the table of contents names, not currentChapterTitle()'s "unnamed".
    std::string chapter;
    if (epub) {
      const int tocIndex = currentTocIndex();
      if (tocIndex != -1) chapter = epub->getTocItem(tocIndex).title;
    }
    info.chapterTitle = chapter.c_str();
    info.pageInChapter = currentPage;
    perfLookup += millis() - perfLookupStart;
    [[maybe_unused]] const auto perfChromeStart = millis();
    readerChrome::draw(renderer, mode,
                       readingMode::textBox(renderer.getScreenWidth(), renderer.getScreenHeight(), textMargins()),
                       info);
    perfChrome = millis() - perfChromeStart;
  }
  if (!readingMode::showsFooter(mode)) {
    RICKY_PERF("status", "lookup=%lu chrome=%lu bar=0 total=%lu", perfLookup, perfChrome, millis() - perfStart);
    return;
  }
  [[maybe_unused]] const auto perfFooterStart = millis();
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = currentTocIndex();
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  if (UiHighDpiProfile::enabled && (!automaticPageTurnActive || sb.textLaneVisible())) {
    if (epub && sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE && !automaticPageTurnActive &&
        title != epub->getTitle()) {
      // Bound the new display strings by pixels, not untrusted metadata length.
      const int column = renderer.getScreenWidth() / 4;
      auto book = renderer.truncatedText(RickyDesignUi::BODY, epub->getTitle().c_str(), column);
      auto chapter = renderer.truncatedText(RickyDesignUi::BODY, title.c_str(), column);
      title = book + " · " + chapter;
    }
    [[maybe_unused]] const auto perfBarStart = millis();
    perfLookup += perfBarStart - perfFooterStart;
    GUI.drawRickyReaderChrome(renderer, bookProgress, currentPage, pageCount, title.c_str(), currentPageBookmarked,
                              section ? section->isBuilding() : false, readingMode::showsHeader(mode));
    RICKY_PERF("status", "lookup=%lu chrome=%lu bar=%lu total=%lu", perfLookup, perfChrome, millis() - perfBarStart,
               millis() - perfStart);
    return;
  }
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
// Text panel rows. RickyOS adds stroke weight beside the size and anti-aliasing
// at the end: readers look for them next to the font, not three levels down in Settings.
constexpr int kTextRowFont = 0;
constexpr int kTextRowSize = 1;
constexpr int kTextRowWeight = 2;
constexpr int kTextRowFirstLayout = 3;
constexpr int kTextRowSpacing = kTextRowFirstLayout;
constexpr int kTextRowAlignment = kTextRowFirstLayout + 1;
constexpr int kTextRowIndent = kTextRowFirstLayout + 2;
constexpr StrId kTextRowNames[] = {StrId::STR_FONT,         StrId::STR_FONT_SIZE,      StrId::STR_RICKY_WEIGHT,
                                   StrId::STR_LINE_SPACING, StrId::STR_PARA_ALIGNMENT, StrId::STR_FIRST_LINE_INDENT,
                                   StrId::STR_TEXT_AA};
constexpr int kTextRowAntiAliasing = kTextRowIndent + 1;
constexpr StrId kReadingModeDogIds[] = {StrId::STR_READING_MODE_ORIGINAL, StrId::STR_READING_MODE_PUPPY,
                                        StrId::STR_READING_MODE_IMMERSIVE, StrId::STR_READING_MODE_ANCIENT};
constexpr StrId kReadingModeCatIds[] = {StrId::STR_READING_MODE_ORIGINAL, StrId::STR_READING_MODE_KITTY,
                                        StrId::STR_READING_MODE_IMMERSIVE, StrId::STR_READING_MODE_ANCIENT};
static_assert(std::size(kReadingModeDogIds) == static_cast<size_t>(readingMode::Mode::Count), "reading mode labels");
static_assert(std::size(kReadingModeCatIds) == std::size(kReadingModeDogIds), "reading mode labels");
const StrId* readingModeIds() { return RickyCompanion::cat() ? kReadingModeCatIds : kReadingModeDogIds; }
// 关, then readingGuideLine::kMenuStyles[1..].
constexpr StrId kReadingLineIds[] = {StrId::STR_STATE_OFF,     StrId::STR_SOLID_LINE, StrId::STR_DASH_LINE,
                                     StrId::STR_DOTTED_LINE,   StrId::STR_WAVY_LINE,  StrId::STR_DOUBLE_LINE,
                                     StrId::STR_HIGHLIGHT_LINE};
static_assert(std::size(kReadingLineIds) == readingGuideLine::kMenuChoices, "reading line labels");
// Body-text weight (readerTextWeightCurve); the middle step is the font as drawn.
constexpr StrId kWeightIds[] = {StrId::STR_RICKY_WEIGHT_THINNEST, StrId::STR_RICKY_WEIGHT_THIN,
                                StrId::STR_FAKE_BOLD_STANDARD, StrId::STR_RICKY_WEIGHT_BOLD,
                                StrId::STR_RICKY_WEIGHT_BOLDEST};
static_assert(std::size(kWeightIds) == CrossPointSettings::RICKY_TEXT_WEIGHT_COUNT, "weight labels");
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr StrId kIndentIds[] = {StrId::STR_FIRST_LINE_INDENT_AUTO, StrId::STR_FIRST_LINE_INDENT_INDENT,
                                StrId::STR_FIRST_LINE_INDENT_NO_INDENT};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Read Pico has one product menu, including cards/upgrades whose saved
  // readerMenuStyle was Toolbar. Do not mutate or reset reader preferences.
  if (UiHighDpiProfile::enabled) return false;
  // Both board classes drive the same chrome: touch through the FreeInkUI tap
  // targets, buttons through the focused-tool pill and the panel cursor.
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

int EpubReaderActivity::currentTocIndex() const {
  if (!epub) return -1;
  const auto offset = section ? currentPageVisibleOffset : cachedVisibleTextOffset;
  return getChapterTocIndex(*epub, currentSpineIndex, offset);
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = currentTocIndex();
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
  switch (row) {
    case kTextRowFont:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case kTextRowSize:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case kTextRowSpacing:
      return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case kTextRowAlignment:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    case kTextRowIndent:
      return I18N.get(kIndentIds[SETTINGS.firstLineIndent < std::size(kIndentIds) ? SETTINGS.firstLineIndent : 0]);
    case kTextRowAntiAliasing:
      return SETTINGS.textAntiAliasing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case kTextRowWeight:
      return I18N.get(kWeightIds[SETTINGS.rickyTextWeight % std::size(kWeightIds)]);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case kTextRowSize: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        if (SETTINGS.fontPointSize == sizes[idx]) return;
        SETTINGS.fontPointSize = sizes[idx];
        SETTINGS.sdFontFlashPreload = 0;
        applyTextSettingLive();
      });
      break;
    }
    case kTextRowSpacing:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case kTextRowAlignment:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case kTextRowIndent:
      overlayPopup.show(StrId::STR_FIRST_LINE_INDENT, kIndentIds, static_cast<int>(std::size(kIndentIds)),
                        SETTINGS.firstLineIndent, [this](int idx) {
                          SETTINGS.firstLineIndent = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case kTextRowWeight:
      overlayPopup.show(StrId::STR_RICKY_WEIGHT, kWeightIds, static_cast<int>(std::size(kWeightIds)),
                        SETTINGS.rickyTextWeight % std::size(kWeightIds), [this](int idx) {
                          SETTINGS.rickyTextWeight = static_cast<uint8_t>(idx);
                          // Weight is drawn, not laid out: redraw the page, no re-pagination.
                          SETTINGS.saveToFile();
                          discardOverlayPage();
                          requestUpdate();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

// Push freshly painted overlay chrome. Where the panel supports it the refresh
// is fired deferred: the loop keeps polling input while the waveform runs, so
// the chrome answers taps and buttons the moment it is visible instead of only
// after a blocking displayBuffer() returns. Caller must hold the RenderLock.
void EpubReaderActivity::pushOverlayRefresh() {
  if (renderer.supportsAsyncRefresh()) {
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    overlayRefreshPending = true;
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Wait out a pending deferred overlay refresh and reseed the panel's
// differential baseline from the framebuffer: the shadow-free async path skips
// the post-refresh resync, so without this the next FAST diff would run
// against the frame from before the chrome and leave stale pixels on the
// glass. Caller must hold the RenderLock.
void EpubReaderActivity::settleOverlayRefresh() {
  if (!overlayRefreshPending) return;
  overlayRefreshPending = false;
  renderer.cleanupGrayscaleWithFrameBuffer();  // waits, then reseeds the baseline
}

void EpubReaderActivity::openOverlay(Overlay target) {
  mappedInput.resetHomeButtonInput();
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
  if (!toolbarUi) {
    LOG_ERR("ERS", "OOM allocating reader toolbar");
    overlay = Overlay::None;
    return;
  }
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents:
      panelIndex = std::max(0, currentTocIndex());
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    case Overlay::Bookmarks:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::Text:
      static_assert(sizeof(SETTINGS.sdFontFamilyName) == 32);
      fontPreview.begin(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize, SETTINGS.sdFontFlashPreload != 0);
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  if (section) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    settleOverlayRefresh();
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    pushOverlayRefresh();
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  mappedInput.resetHomeButtonInput();
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  // A panel row toggled in place (image scaling) applies here and only here:
  // the snapshot below holds pixels decoded with the old filter, so drop it to
  // fall through to the re-render path instead of restoring them.
  if (imageScalingDirty) {
    imageScalingDirty = false;
    discardOverlayPage();
  }
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
  if (!epub || !section || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(section->currentPage + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents || overlay == Overlay::Bookmarks) {
    model.segmentLabels[0] = tr(STR_TOOL_CONTENTS);
    model.segmentLabels[1] = tr(STR_BOOKMARKS);
    model.activeSegment = overlay == Overlay::Bookmarks ? 1 : 0;
    model.minRows = std::max(epub->getTocItemsCount(), bookmarkRowCount());
  }
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Bookmarks) {
    model.panelTitle = tr(STR_BOOKMARKS);
    model.itemCount = bookmarkRowCount();
    model.rowText = [this](int i) { return bookmarkRowName(i); };
    model.rowValue = [this](int i) { return bookmarkRowValue(i); };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
    model.rowCheckboxContext = this;
    model.rowCheckbox = [](void*, int i, freeink::ui::ListItem& item) {
      if (i == kTextRowAntiAliasing) GUI.setCheckboxRow(item, SETTINGS.textAntiAliasing);
    };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
    model.rowCheckboxContext = this;
    model.rowCheckbox = [](void* ctx, int i, freeink::ui::ListItem& item) {
      const auto* self = static_cast<EpubReaderActivity*>(ctx);
      using MA = EpubReaderMenuActivity::MenuAction;
      const auto action = self->moreItems[i].action;
      if (action == MA::NIGHT_MODE) GUI.setCheckboxRow(item, SETTINGS.screenInverted);
      if (action == MA::FRONTLIGHT) GUI.setCheckboxRow(item, Frontlight.isOn());
    };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      settleOverlayRefresh();
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        pushOverlayRefresh();
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    renderOverlay();
    pushOverlayRefresh();
  };

  const bool textBook = Txt::isTxtOrMd(epub->getPath());
  const int chapterCount = textBook ? epub->getTocItemsCount() : epub->getSpineItemsCount();
  const int chapterIndex = textBook ? std::max(0, currentTocIndex()) : currentSpineIndex;
  const auto gotoChapter = [this, textBook, chapterCount, chapterIndex](int target) {
    target = std::clamp(target, 0, chapterCount - 1);
    if (target != chapterIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      if (textBook) {
        const auto item = epub->getTocItem(target);
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
      } else {
        currentSpineIndex = target;
      }
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0 ? Overlay::Contents : (tool == 1 ? Overlay::Text : Overlay::More);
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoChapter(chapterIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoChapter(chapterIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoChapter(static_cast<int>(
            (static_cast<float>(routed.permille) / 1000.0f) * static_cast<float>(chapterCount - 1) + 0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 2) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoChapter(chapterIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents    ? epub->getTocItemsCount()
                    : overlay == Overlay::Bookmarks ? bookmarkRowCount()
                    : overlay == Overlay::Text      ? kTextRowCount
                                                    : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == kTextRowFont) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        {
          RenderLock lock;
          settleOverlayRefresh();
        }
        auto textSettings = makeUniqueNoThrow<TextSettingsActivity>(
            renderer, mappedInput, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family,
            TextSettingsActivity::InitialFontState::Unchanged, TextSettingsActivity::StartMode::PreviewOnly);
        if (!textSettings) {
          LOG_ERR("ERS", "OOM allocating text settings");
          overlay = Overlay::Text;
          return;
        }
        startActivityForResult(std::move(textSettings), [this](const ActivityResult&) {
          applyReaderTextSettings();
          overlay = Overlay::Text;  // back to the Text panel
          panelIndex = 0;
          if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
          requestUpdate();                    // re-render page + Text panel
        });
      } else if (panelIndex == kTextRowAntiAliasing) {
        // Same layout either way: only the page's rendering changes, so no re-pagination.
        SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;
        SETTINGS.saveToFile();
        discardOverlayPage();
        requestUpdate();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::Bookmarks) {
      activateBookmarkRow(panelIndex);
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        settleOverlayRefresh();
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Segment: {
      // Contents | Bookmarks titles: the same sheet, another list.
      const Overlay target = routed.value == 1 ? Overlay::Bookmarks : Overlay::Contents;
      if (target != overlay) openOverlay(target);
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  settleOverlayRefresh();
  overlayPopup.render(renderer);
  pushOverlayRefresh();
}

void EpubReaderActivity::applyReaderTextSettings() {
  SETTINGS.saveToFile();
  RenderLock lock;
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  sdFontSystem.ensureLoaded(renderer);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();  // force re-pagination with the new settings
}

// A .cpfont accelerates one size per file, so a size picked in the reader menu drops the
// flash source; a vector font's cache holds the whole file and comes back unchanged. Ask
// as text settings would when the new size can be accelerated.
void EpubReaderActivity::applyFontSizeFromMenu(const bool mayAsk) {
  SETTINGS.sdFontFlashPreload = 0;
  applyReaderTextSettings();
  requestUpdate();
  if (SETTINGS.sdFontFamilyName[0] == '\0') return;
  const auto* family = sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName);
  const auto* file = family ? family->findNearestSize(SETTINGS.fontPointSize) : nullptr;
  const auto check = file ? SdCardFontCache::preflight(file->path.c_str()) : SdCardFontCache::Result::InvalidFont;
  LOG_INF("ERS", "Size %u from the menu: acceleration %s", static_cast<unsigned>(SETTINGS.fontPointSize),
          SdCardFontCache::resultName(check));
  if (check == SdCardFontCache::Result::AlreadyCached) {
    SETTINGS.sdFontFlashPreload = 1;
    SETTINGS.saveToFile();
    RenderLock lock;
    sdFontSystem.releaseLoadedFont(renderer, /*includeVector=*/true);
    sdFontSystem.ensureLoaded(renderer);
    section.reset();
    return;
  }
  if (check != SdCardFontCache::Result::Ok || !mayAsk) return;
  startActivityForResultWith<TextSettingsActivity>(
      [this](const ActivityResult&) {
        READING_STATS.resumeSession();
        applyReaderTextSettings();
        requestUpdate();
      },
      &sdFontSystem.registry(), TextSettingsActivity::Tab::Family, TextSettingsActivity::InitialFontState::Changed,
      TextSettingsActivity::StartMode::AskThenExit);
}

void EpubReaderActivity::finishFontPreview() {
  const auto* family = sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName);
  const auto* file = family ? family->findNearestSize(SETTINGS.fontPointSize) : nullptr;
  // Vector (TTF/OTF) families are accelerated too (their regular file is cached).
  constexpr bool supportsPreload = true;
  const auto check =
      file && supportsPreload ? SdCardFontCache::preflight(file->path.c_str()) : SdCardFontCache::Result::InvalidFont;
  const auto decision = fontPreview.finish(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize,
                                           check == SdCardFontCache::Result::AlreadyCached, supportsPreload);
  switch (decision) {
    case ReaderFontPreview::Decision::Keep:
      return;
    case ReaderFontPreview::Decision::Enable: {
      RenderLock lock;
      const bool reload = SETTINGS.sdFontFlashPreload == 0;
      SETTINGS.sdFontFlashPreload = 1;
      // ensureLoaded's same-family/size fast path does not change the source.
      if (reload) sdFontSystem.releaseLoadedFont(renderer, /*includeVector=*/true);
      sdFontSystem.ensureLoaded(renderer);
      break;
    }
    case ReaderFontPreview::Decision::Disable:
    case ReaderFontPreview::Decision::Ask:
      SETTINGS.sdFontFlashPreload = 0;
      break;
  }
  SETTINGS.saveToFile();
  if (decision != ReaderFontPreview::Decision::Ask) return;

  if (check == SdCardFontCache::Result::TooLarge) {
    {
      RenderLock lock;
      fontPromptState = FontPromptState::TooLarge;
    }
    requestUpdateAndWait();
    fontNoticeStartedAt = millis();
    return;
  }
  {
    RenderLock lock;
    fontPromptState = FontPromptState::Asking;
    fontPromptWaitForBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
    if (check == SdCardFontCache::Result::Ok) {
      constexpr StrId options[] = {StrId::STR_FONT_PRELOAD_START, StrId::STR_FONT_PRELOAD_SKIP};
      overlayPopup.show(StrId::STR_FONT_PRELOAD_CONFIRM, options, static_cast<int>(std::size(options)), 0,
                        [this](int index) {
                          RenderLock lock;
                          if (index == 0) fontPromptState = FontPromptState::Accepted;
                        });
    } else {
      constexpr StrId options[] = {StrId::STR_OK_BUTTON};
      overlayPopup.show(fontpreload::failureMessage(check), options, static_cast<int>(std::size(options)), 0, {});
    }
  }
  requestUpdate();
}

void EpubReaderActivity::handleFontPreloadPrompt() {
  if (fontPromptState == FontPromptState::TooLarge) {
    if (millis() - fontNoticeStartedAt >= fontpreload::NOTICE_DURATION_MS) {
      for (uint8_t button = 0; button < MappedInputManager::kButtonCount; ++button) {
        if (mappedInput.isPressed(static_cast<MappedInputManager::Button>(button))) return;
      }
      int x = 0, y = 0;
      if (mappedInput.isScreenTouchHeld(x, y)) return;
      {
        RenderLock lock;
        fontPromptState = FontPromptState::Idle;
      }
      requestUpdate();
    }
    return;
  }
  if (fontPromptWaitForBackRelease) {
    fontPromptWaitForBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
    return;  // Consume the inherited release as well as the hold.
  }
  overlayPopup.handleInput(mappedInput, [this] { requestUpdate(); });
  if (overlayPopup.isActive()) return;

  const bool accepted = fontPromptState == FontPromptState::Accepted;
  {
    RenderLock lock;
    fontPromptState = FontPromptState::Idle;
    overlayPopup.dismiss();
  }
  if (!accepted) {
    requestUpdate();
    return;
  }

  // ActivityManager owns this existing progress activity across loop frames;
  // stack storage cannot outlive this call. No second font/cache buffer is added.
  auto preload = makeUniqueNoThrow<TextSettingsActivity>(
      renderer, mappedInput, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family,
      TextSettingsActivity::InitialFontState::Changed, TextSettingsActivity::StartMode::PreloadThenExit);
  if (!preload) {
    LOG_ERR("ERS", "OOM allocating font preload activity (%zu bytes)", sizeof(TextSettingsActivity));
    SETTINGS.sdFontFlashPreload = 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(preload), [this](const ActivityResult&) {
    {
      RenderLock lock;
      // Also covers a cache that became valid between the prompt and preload.
      sdFontSystem.releaseLoadedFont(renderer, /*includeVector=*/true);
      sdFontSystem.ensureLoaded(renderer);
    }
    lastPageTurnTime = millis();
    requestUpdate();
  });
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   // Bookmarks live beside the contents (Contents | Bookmarks).
                                   if (item.action == MA::BOOKMARKS || item.action == MA::TOGGLE_BOOKMARK) return true;
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
}

int EpubReaderActivity::bookmarkRowCount() const {
  const int saved = static_cast<int>(cachedBookmarks.size());
  return 1 + saved + (saved > 0 ? 1 : 0);
}

std::string EpubReaderActivity::bookmarkRowName(const int row) const {
  const int saved = static_cast<int>(cachedBookmarks.size());
  if (row == 0) return currentPageBookmarked ? tr(STR_RICKY_BOOKMARK_REMOVE) : tr(STR_RICKY_BOOKMARK_ADD);
  if (row >= 1 && row <= saved) {
    const auto& bookmark = cachedBookmarks[row - 1];
    if (!bookmark.name.empty()) return bookmark.name;
    // Re-sanitized for bookmarks saved before CJK summaries dropped their spaces.
    return bookmark.summary.empty() ? std::string(tr(STR_UNNAMED))
                                    : BookmarkUtil::sanitizeBookmarkSummary(bookmark.summary);
  }
  return row == saved + 1 ? tr(STR_RICKY_BOOKMARK_MANAGE) : "";
}

std::string EpubReaderActivity::bookmarkRowValue(const int row) const {
  if (row < 1 || row > static_cast<int>(cachedBookmarks.size())) return "";
  const float percentage = std::clamp(cachedBookmarks[row - 1].percentage, 0.0f, 1.0f);
  return std::to_string(static_cast<int>(percentage * 100.0f + 0.5f)) + "%";
}

void EpubReaderActivity::activateBookmarkRow(const int row) {
  const int saved = static_cast<int>(cachedBookmarks.size());
  if (row == 0) {
    // Toggle this page; the panel stays open and its first row flips. The stored
    // page lacks the new status-bar mark, so let renderBook() redraw under it.
    addBookmark();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  // Leaving the panel for the page (a jump) or the full list (rename / delete).
  overlay = Overlay::None;
  overlayPopup.dismiss();
  discardOverlayPage();
  {
    RenderLock lock;
    settleOverlayRefresh();
  }
  if (row <= saved) {
    const auto& bookmark = cachedBookmarks[row - 1];
    ProgressChangeResult jump;
    jump.xpath = bookmark.xpath;
    jump.percentage = bookmark.percentage;
    jump.hasSavedProgress = true;
    jump.hasVisibleTextOffset = bookmark.hasVisibleTextOffset;
    jump.visibleTextOffset = bookmark.visibleTextOffset;
    jump.spineIndex = bookmark.computedSpineIndex;
    if (bookmark.computedChapterPageCount > 0 && bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
        bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
      jump.page = bookmark.computedChapterProgress;
      jump.totalPages = bookmark.computedChapterPageCount;
    }
    applyProgressChange(jump);
    return;
  }
  startActivityForResultWith<EpubReaderBookmarksActivity>(
      [this](const ActivityResult& result) {
        loadCachedBookmarks();
        updateBookmarkFlag();
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        applyProgressChange(std::get<ProgressChangeResult>(result.data));
      },
      epub, epub->getPath());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::READING_MODE:
      return I18N.get(readingModeIds()[static_cast<uint8_t>(readingMode::modeFor(SETTINGS.readingMode))]);
    case MA::READING_LINE:
      return I18N.get(kReadingLineIds[readingGuideLine::menuIndexFor(SETTINGS.readingGuideLineEnabled,
                                                                     SETTINGS.readingGuideLineStyle)]);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::IMAGE_SCALING: {
      // Same labels the reader menu's option popup offers, so the row's value
      // and the popup's highlighted entry always agree.
      static constexpr StrId kScaling[] = {StrId::STR_IMAGE_SCALING_NEAREST, StrId::STR_IMAGE_SCALING_BILINEAR};
      static_assert(std::size(kScaling) == CrossPointSettings::IMAGE_SCALING_COUNT, "image scaling labels");
      const size_t mode = SETTINGS.imageScaling < CrossPointSettings::IMAGE_SCALING_COUNT ? SETTINGS.imageScaling : 0;
      return I18N.get(kScaling[mode]);
    }
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  {
    RenderLock lock;  // several actions launch screens that paint the framebuffer
    settleOverlayRefresh();
  }
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(PAGE_TURN_RATES[idx]));
      });
      paintOverlayPopup();
      return;
    }
    case MA::IMAGE_SCALING: {
      // A plain on/off-style row like the frontlight one: one tap cycles, no
      // picker. The value only reaches the page when the overlay closes, so the
      // row repaints the panel over the page snapshot instead of re-rendering
      // the page (which would decode the image twice in a row).
      const uint8_t current =
          SETTINGS.imageScaling < CrossPointSettings::IMAGE_SCALING_COUNT ? SETTINGS.imageScaling : 0;
      const uint8_t next = static_cast<uint8_t>((current + 1) % CrossPointSettings::IMAGE_SCALING_COUNT);
      SETTINGS.imageScaling = next;
      SETTINGS.saveToFile();
      imageScalingDirty = true;
      LOG_INF("ERS", "Image scaling -> %s",
              next == CrossPointSettings::IMAGE_SCALING_BILINEAR ? "bilinear" : "nearest");
      RenderLock lock;  // the render task shares the framebuffer
      if (overlayPageStored) {
        // Clean page back first (the sheet's old value text is not
        // background-filled), then re-snapshot it and lay the panel on top.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      renderOverlay();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::READING_MODE:
      overlayPopup.show(StrId::STR_READING_MODE, readingModeIds(), static_cast<int>(std::size(kReadingModeDogIds)),
                        static_cast<int>(readingMode::modeFor(SETTINGS.readingMode)), [this](int idx) {
                          if (SETTINGS.readingMode == idx) return;
                          SETTINGS.readingMode = static_cast<uint8_t>(idx);
                          // The frame and the header/footer bands move the text column: re-paginate.
                          applyTextSettingLive();
                        });
      paintOverlayPopup();
      return;
    case MA::READING_LINE:
      overlayPopup.show(
          StrId::STR_READING_GUIDE_LINE, kReadingLineIds, static_cast<int>(std::size(kReadingLineIds)),
          readingGuideLine::menuIndexFor(SETTINGS.readingGuideLineEnabled, SETTINGS.readingGuideLineStyle),
          [this](int idx) {
            if (idx < 0 || idx >= readingGuideLine::kMenuChoices) return;
            SETTINGS.readingGuideLineEnabled = idx == 0 ? 0 : 1;
            if (idx != 0) {
              SETTINGS.readingGuideLineStyle = static_cast<uint8_t>(readingGuideLine::kMenuStyles[idx]);
            }
            // Drawn, not laid out: redraw the page under the panel, no re-pagination.
            SETTINGS.saveToFile();
            discardOverlayPage();
            requestUpdate();
          });
      paintOverlayPopup();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  if (action == MA::GO_TO_PERCENT && overlayPageStored) {
    // The percent dialog is a popup over the current frame: wipe the toolbar
    // chrome back to the clean page first so the dialog draws over the page,
    // not the sheet. No refresh push — the dialog's first frame carries it.
    RenderLock lock;
    settleOverlayRefresh();
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
  } else {
    discardOverlayPage();
  }
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage, currentPageVisibleOffset};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    return;
  }

  if (savePosition) {
    // Full: drop the oldest entry so Back always returns from the newest jump.
    if (footnoteDepth == MAX_FOOTNOTE_DEPTH) {
      std::copy(savedPositions + 1, savedPositions + MAX_FOOTNOTE_DEPTH, savedPositions);
      footnoteDepth--;
    }
    // A child screen (menu, footnote list) may have released the section;
    // nextPageNumber then holds the page it was on.
    const int page = section ? section->currentPage : nextPageNumber;
    savedPositions[footnoteDepth] = {currentSpineIndex, page};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, page);
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::saveLinkStack() const {
  // [depth] then depth x (spine u16 LE, page u16 LE)
  uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
  size_t size = 0;
  data[size++] = static_cast<uint8_t>(footnoteDepth);
  for (int i = 0; i < footnoteDepth; i++) {
    const SavedPosition& pos = savedPositions[i];
    data[size++] = pos.spineIndex & 0xFF;
    data[size++] = (pos.spineIndex >> 8) & 0xFF;
    data[size++] = pos.pageNumber & 0xFF;
    data[size++] = (pos.pageNumber >> 8) & 0xFF;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub->getCachePath() + "/links.bin", f)) return;
  if (f.write(data, size) != size) LOG_ERR("ERS", "Failed to write link stack");
}

void EpubReaderActivity::loadLinkStack() {
  const std::string path = epub->getCachePath() + "/links.bin";
  if (!Storage.exists(path.c_str())) return;
  {
    HalFile f;
    uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
    if (Storage.openFileForRead("ERS", path, f)) {
      const int size = f.read(data, sizeof(data));
      const int depth = size > 0 ? data[0] : 0;
      if (depth >= 1 && depth <= MAX_FOOTNOTE_DEPTH && size == 1 + depth * 4) {
        const int spineCount = epub->getSpineItemsCount();
        bool valid = true;
        for (int i = 0; i < depth; i++) {
          const uint8_t* p = data + 1 + i * 4;
          savedPositions[i] = {p[0] | (p[1] << 8), p[2] | (p[3] << 8)};
          valid = valid && savedPositions[i].spineIndex < spineCount;
        }
        if (valid) {
          footnoteDepth = depth;
          LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
        }
      }
    }
  }
  // Consumed once: a later exit rewrites it, and an unclean shutdown must not
  // resurrect a stale stack.
  Storage.remove(path.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    pendingAnchor.clear();
    pendingPercentJump = false;
    pendingPageJump.reset();
    pendingOffsetJump = pos.visibleTextOffset;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

int EpubReaderActivity::getProgressBasisPoints() const {
  if (isAtEndOfBook()) return 10000;
  if (!epub || !section || epub->getBookSize() == 0) return getProgressPercent() * 100;
  const int totalPages = section->estimatedTotalPages();
  if (totalPages <= 0) return getProgressPercent() * 100;
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  const int basisPoints =
      static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 10000.0f + 0.5f);
  return std::clamp(basisPoints, 0, 10000);
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  localPos.hasResolvedSpineIndex = true;
  localPos.hasMappedPage = true;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
