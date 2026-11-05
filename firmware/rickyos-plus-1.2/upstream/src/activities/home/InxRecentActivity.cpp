#include "InxRecentActivity.h"

#include <BoardConfig.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "activities/apps/reading-stats/RickyBadgesActivity.h"
#include "components/RickyDesignTokens.h"
#include "components/RickyHomeLayout.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/themes/inx/InxTheme.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"
#include "util/PanelTileCache.h"
#include "util/ReadingStatsAnalytics.h"

namespace {
constexpr int kGap = UiHighDpiProfile::enabled ? UiHighDpiProfile::controlGap : 8;
constexpr int kPagePadding = UiHighDpiProfile::enabled ? UiHighDpiProfile::contentPadding : 18;
constexpr int kProgressHeight = 6;

const char* titleOf(const RecentBook& book) { return book.title.empty() ? book.path.c_str() : book.title.c_str(); }

bool drawRecentCover(const GfxRenderer& renderer, const Bitmap& bitmap, const Rect& bounds) {
  const auto fit = RickyHomeLayout::fitBitmap(bitmap.getWidth(), bitmap.getHeight(), bounds.width, bounds.height);
  if (fit.width <= 0 || fit.height <= 0) return false;
  const int x = bounds.x + (bounds.width - fit.width) / 2;
  const int y = bounds.y + (bounds.height - fit.height) / 2;
  if (renderer.isGrayscale16Active() && !bitmap.is1Bit())
    return renderer.drawBitmapGrayscale16(bitmap, x, y, fit.width, fit.height);
  return renderer.drawBitmap(bitmap, x, y, fit.width, fit.height);
}

void drawMiniProgress(const GfxRenderer& renderer, const Rect rect, const uint8_t percent) {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int innerWidth = std::max(0, rect.width - 2);
  const int fillWidth = innerWidth * std::min<int>(percent, 100) / 100;
  if (fillWidth > 0) renderer.fillRect(rect.x + 1, rect.y + 1, fillWidth, std::max(0, rect.height - 2));
}

uint8_t progressOf(const ReadingBookStats* stats) { return stats ? stats->lastProgressPercent : 0; }

void drawSparseInk(const GfxRenderer& renderer, const Rect rect) {
  for (int y = rect.y; y < rect.y + rect.height; y += 2) {
    for (int x = rect.x; x < rect.x + rect.width; x += 2) renderer.drawPixel(x, y, true);
  }
}

void drawDottedSeparator(const GfxRenderer& renderer, const int x, const int y, const int width) {
  for (int px = x; px < x + width; px += 3) renderer.drawPixel(px, y, true);
}

Rect fitCoverRect(const Rect bounds) {
  const auto size = InxCoverGeometry::fit(bounds.width, bounds.height);
  return Rect{bounds.x + (bounds.width - size.width) / 2, bounds.y + (bounds.height - size.height) / 2, size.width,
              size.height};
}

void drawThickFrame(const GfxRenderer& renderer, const Rect rect, const int thickness = 3) {
  for (int inset = 0; inset < thickness; ++inset) {
    renderer.drawRect(rect.x - inset, rect.y - inset, rect.width + inset * 2, rect.height + inset * 2, true);
  }
}

void drawProgressBadge(const GfxRenderer& renderer, const Rect cover, const uint8_t progress) {
  char label[8];
  snprintf(label, sizeof(label), "%u%%", static_cast<unsigned>(progress));
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  const int textHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int width = std::max(24, textWidth + 8);
  const int height = textHeight + 4;
  const int x = cover.x + cover.width - width - 2;
  const int y = cover.y + 2;
  renderer.fillRect(x, y, width, height, true);
  renderer.drawText(SMALL_FONT_ID, x + (width - textWidth) / 2, y + 2, label, false, EpdFontFamily::BOLD);
}

void drawMetric(const GfxRenderer& renderer, const int x, const int y, const char* value, const char* label,
                const int width) {
  const std::string shownValue = renderer.truncatedText(UI_12_FONT_ID, value, width, EpdFontFamily::BOLD);
  const std::string shownLabel = renderer.truncatedText(SMALL_FONT_ID, label, width);
  renderer.drawText(UI_12_FONT_ID, x, y, shownValue.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, x, y + renderer.getLineHeight(UI_12_FONT_ID) + 4, shownLabel.c_str());
}

void drawBookText(const GfxRenderer& renderer, const RecentBook& book, const int x, const int y, const int width,
                  const bool author) {
  const std::string title = renderer.truncatedText(UI_10_FONT_ID, titleOf(book), width, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  if (author && !book.author.empty()) {
    const std::string subtitle = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), width);
    renderer.drawText(SMALL_FONT_ID, x,
                      y + (UiHighDpiProfile::enabled ? renderer.getLineHeight(UI_10_FONT_ID) + 6 : 27),
                      subtitle.c_str());
  }
}
}  // namespace

void InxRecentActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  selected = MainTabs::contentEdgeIndex(edge, books ? static_cast<int>(books->size()) : 0);
}

InxRecentLayout InxRecentActivity::layout() const {
  const auto value = static_cast<InxRecentLayout>(SETTINGS.inxRecentLayout);
  return value < InxRecentLayout::Count ? value : InxRecentLayout::Flow;
}

Rect InxRecentActivity::contentRect() const {
  Rect content = pageContentRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topInset = mainTabsAtBottom() && !hasMainTabStatusBar() ? metrics.batteryBarHeight : 0;
  content.y += topInset + metrics.verticalSpacing;
  content.height -= topInset + metrics.verticalSpacing * 2;
  if (!mainTabsAtBottom()) {
    content.height -= std::max(0, InxRecentGeometry::footerReservedHeight - metrics.buttonHintsHeight);
  }
  return content;
}

const ReadingBookStats* InxRecentActivity::statsAt(const int index) const {
  return index >= 0 && index < static_cast<int>(bookStats.size()) ? bookStats[index] : nullptr;
}

void InxRecentActivity::onEnter() {
  Activity::onEnter();
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  books = &RECENT_BOOKS.getBooks();
  bookStats.fill(nullptr);
  for (size_t i = 0; i < std::min(books->size(), bookStats.size()); ++i) {
    const RecentBook& book = (*books)[i];
    bookStats[i] = READING_STATS.findMatchingBookForPath(book.path, book.title, book.author);
  }
  selected = 0;
  thumbnailHeight = 0;
  preparedHeights.fill(0);
  targetCoverStates.fill(CoverCacheState::Unchecked);
  fallbackCoverStates.fill(CoverCacheState::Unchecked);
  if (layout() == InxRecentLayout::Flow) {
    // One optional host per live home; interaction tables/props cannot live on
    // the render stack. Borrow recents and the existing bounded cover cache.
    rickyHome = makeUniqueNoThrow<RickyHomeUi>(renderer);
    if (rickyHome) {
      rickyHome->begin(
          *books,
          [](void* owner, int index, const Rect& bounds) {
            auto& activity = *static_cast<InxRecentActivity*>(owner);
            activity.setThumbnailHeight(activity.rickyHome->coverHeight());
            return activity.drawBookCover(index, bounds);  // The product host owns the title-only fallback.
          },
          this);
    } else {
      LOG_ERR("RICKY", "OOM: home host; retaining original home layout");
    }
  }
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  clearCoverCaches();
#endif
  requestUpdate();
}

void InxRecentActivity::onExit() {
  if (rickyHome) rickyHome->closeRouting();
  rickyHome.reset();
  books = nullptr;
  bookStats.fill(nullptr);
  targetCoverStates.fill(CoverCacheState::Unchecked);
  fallbackCoverStates.fill(CoverCacheState::Unchecked);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  clearCoverCaches();
#endif
  thumbnailHeight = 0;
  Activity::onExit();
}

void InxRecentActivity::openSelected() {
  if (!books || selected < 0 || selected >= static_cast<int>(books->size())) return;
  onSelectBook((*books)[selected].path);
}

void InxRecentActivity::setThumbnailHeight(const int displayHeight) {
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  const int height = std::max(0, displayHeight);
#else
  const int height = InxCoverGeometry::thumbnailHeightForCropFill(displayHeight);
#endif
  if (thumbnailHeight == height) return;
  thumbnailHeight = height;
  targetCoverStates.fill(CoverCacheState::Unchecked);
  fallbackCoverStates.fill(CoverCacheState::Unchecked);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  clearCoverCaches();
#endif
}

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
void InxRecentActivity::clearCoverCaches() {
  std::for_each(targetCoverCaches.begin(), targetCoverCaches.end(), [](auto& cache) { cache = {}; });
  std::for_each(fallbackCoverCaches.begin(), fallbackCoverCaches.end(), [](auto& cache) { cache = {}; });
  cachedCoverBytes = 0;
}

InxRecentActivity::CoverCacheLoadResult InxRecentActivity::tryLoadCoverCache(HalFile& file, CoverRamCache& cache) {
  if (cache.attempted) return CoverCacheLoadResult::Stream;
  cache.attempted = true;

  const size_t fileSize = file.fileSize();
  if (fileSize == 0 || fileSize > MAX_CACHED_COVER_FILE_BYTES || cachedCoverBytes > MAX_COVER_CACHE_BYTES ||
      fileSize > MAX_COVER_CACHE_BYTES - cachedCoverBytes) {
    return CoverCacheLoadResult::Stream;
  }
  if (!memory::psramHasHeadroom(fileSize, fileSize, COVER_CACHE_MAX_ALLOC_RESERVE)) {
    return CoverCacheLoadResult::Stream;
  }

  // A cover is up to 64 KB, so it cannot live on the task stack. Allocate once
  // on first use and retain it for this Activity.
  auto bytes = memory::makePsramByteBufferNoThrow(fileSize);
  if (!bytes) return CoverCacheLoadResult::Stream;
  if (!file.seek(0) || file.read(bytes.get(), fileSize) != static_cast<int>(fileSize)) {
    LOG_ERR("INX", "Short read loading cover into PSRAM (%u bytes)", static_cast<unsigned>(fileSize));
    file.seek(0);
    return CoverCacheLoadResult::Stream;
  }

  Bitmap bitmap(bytes.get(), fileSize);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    return CoverCacheLoadResult::Invalid;
  }

  cache.bytes = std::move(bytes);
  cache.size = fileSize;
  cachedCoverBytes += fileSize;
  return CoverCacheLoadResult::Loaded;
}
#endif

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
bool InxRecentActivity::tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state,
                                         CoverRamCache& cache) {
#else
bool InxRecentActivity::tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state) {
#endif
  switch (state) {
    case CoverCacheState::Unchecked:
      if (!Storage.exists(path.c_str())) {
        state = CoverCacheState::Missing;
        return false;
      }
      state = CoverCacheState::Ready;
      break;
    case CoverCacheState::Ready:
      break;
    case CoverCacheState::Missing:
    case CoverCacheState::Unavailable:
      return false;
  }

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  const auto drawCached = [this, &bounds, &state, &cache] {
    Bitmap bitmap(cache.bytes.get(), cache.size);
    if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
        BookCoverLoader::isOutdatedThumbnail(bitmap)) {
      cachedCoverBytes -= cache.size;
      cache = {};
      state = CoverCacheState::Missing;
      return false;
    }
    return drawRecentCover(renderer, bitmap, bounds);
  };

  if (cache.bytes) return drawCached();
#endif

  HalFile file;
  if (!Storage.openFileForRead("INX", path, file)) {
    state = CoverCacheState::Missing;
    return false;
  }
  if (file.fileSize() == 0) {
    state = CoverCacheState::Unavailable;  // BookCoverLoader's marker: this book has no usable cover
    return false;
  }

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  switch (tryLoadCoverCache(file, cache)) {
    case CoverCacheLoadResult::Loaded:
      return drawCached();
    case CoverCacheLoadResult::Invalid:
      state = CoverCacheState::Missing;
      return false;
    case CoverCacheLoadResult::Stream:
      break;
  }
#endif

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      BookCoverLoader::isOutdatedThumbnail(bitmap)) {
    state = CoverCacheState::Missing;  // prepareNextMissingCover() regenerates it
    return false;
  }
  return drawRecentCover(renderer, bitmap, bounds);
}

bool InxRecentActivity::drawBookCover(const int bookIndex, const Rect& bounds) {
  renderer.fillRect(bounds.x, bounds.y, bounds.width, bounds.height, false);
  if (books && bookIndex >= 0 && bookIndex < static_cast<int>(books->size()) &&
      bookIndex < static_cast<int>(RecentBooksStore::MAX_RECENT_BOOKS) && thumbnailHeight > 0) {
    const RecentBook& book = (*books)[bookIndex];
    if (!book.coverBmpPath.empty()) {
      // The bounds were just filled white, so a cover tile depends only on its file and place.
      char place[40];
      snprintf(place, sizeof(place), "|home|%d,%d,%d,%d", bounds.x, bounds.y, bounds.width, bounds.height);
      std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, thumbnailHeight);
      if (PanelTileCache::restore(renderer, path + place)) {
        targetCoverStates[bookIndex] = CoverCacheState::Ready;
        return true;
      }
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
      if (tryDrawBookCover(path, bounds, targetCoverStates[bookIndex], targetCoverCaches[bookIndex])) {
#else
      if (tryDrawBookCover(path, bounds, targetCoverStates[bookIndex])) {
#endif
        PanelTileCache::store(renderer, path + place, path, bounds.x, bounds.y, bounds.width, bounds.height);
        return true;
      }
      if (book.coverBmpPath.find("[HEIGHT]") != std::string::npos &&
          thumbnailHeight != InxMetrics::values.homeCoverHeight) {
        path = UITheme::getCoverThumbPath(book.coverBmpPath, InxMetrics::values.homeCoverHeight);
        if (PanelTileCache::restore(renderer, path + place)) return true;
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
        if (tryDrawBookCover(path, bounds, fallbackCoverStates[bookIndex], fallbackCoverCaches[bookIndex])) {
#else
        if (tryDrawBookCover(path, bounds, fallbackCoverStates[bookIndex])) {
#endif
          PanelTileCache::store(renderer, path + place, path, bounds.x, bounds.y, bounds.width, bounds.height);
          return true;
        }
      }
    }
  }

  const auto size = InxCoverGeometry::fit(bounds.width, bounds.height);
  const int x = bounds.x + (bounds.width - size.width) / 2;
  const int y = bounds.y + (bounds.height - size.height) / 2;
  renderer.drawRect(x, y, size.width, size.height, 2, true);
  renderer.fillRect(x, y + size.height / 3, size.width, size.height * 2 / 3);
  constexpr int iconSize = 32;
  renderer.drawIcon(CoverIcon, x + (size.width - iconSize) / 2, y + size.height / 6 - iconSize / 2, iconSize);
  return false;
}

bool InxRecentActivity::prepareNextMissingCover() {
  if (!books || thumbnailHeight <= 0) return false;
  const size_t count = std::min(books->size(), RecentBooksStore::MAX_RECENT_BOOKS);
  for (size_t index = 0; index < count; ++index) {
    CoverCacheState& state = targetCoverStates[index];
    switch (state) {
      case CoverCacheState::Unchecked: {
        const std::string& templatePath = (*books)[index].coverBmpPath;
        if (templatePath.empty()) {
          state = CoverCacheState::Unavailable;
          continue;
        }
        state = Storage.exists(UITheme::getCoverThumbPath(templatePath, thumbnailHeight).c_str())
                    ? CoverCacheState::Ready
                    : CoverCacheState::Missing;
        if (state == CoverCacheState::Ready) continue;
        break;
      }
      case CoverCacheState::Ready:
      case CoverCacheState::Unavailable:
        continue;
      case CoverCacheState::Missing:
        break;
    }

    if (preparedHeights[index] == thumbnailHeight ||
        !BookCoverLoader::canMakeThumbnail((*books)[index].path, thumbnailHeight)) {
      // Prepared this visit and still not drawable, or nothing to prepare (TXT/MD books
      // keep a thumbnail path too): the placeholder stays, without a repaint.
      state = CoverCacheState::Unavailable;
      continue;
    }
    preparedHeights[index] = static_cast<int16_t>(thumbnailHeight);
    // The frame is already on the panel. The loan frees the framebuffer, so the
    // next frame repaints it; a failure leaves a marker and is not retried.
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      // Draw reads the recents entry's path; a thumbnail written anywhere else is no use.
      const std::string wanted = UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbnailHeight);
      state = BookCoverLoader::ensureThumbnail((*books)[index].path, thumbnailHeight) == wanted
                  ? CoverCacheState::Ready
                  : CoverCacheState::Unavailable;
    }
    requestUpdate();
    return true;
  }
  return false;
}

int InxRecentActivity::indexFromPoint(const int x, const int y) const {
  return InxRecentGeometry::indexFromPoint(contentRect(), x, y, selected, books ? static_cast<int>(books->size()) : 0,
                                           layout());
}

void InxRecentActivity::loop() {
  if (coverWorkPending.exchange(false)) {
    // Waits for the render task to finish the current frame, then prepares at most
    // one missing thumbnail; its update brings the next one.
    RenderLock lock;
    prepareNextMissingCover();
    return;
  }
  if (rickyHome) {
    const int action = rickyHome->selectedAction(mappedInput);
    if (action == RickyHomeUi::LIBRARY) {
      activityManager.goToLibrary();
      return;
    }
    if (action == RickyHomeUi::STATISTICS) {
      activityManager.goToRickyReadingStats();
      return;
    }
    if (action == RickyHomeUi::PROFILE) {
      // The avatar opens the badges; the profile itself is edited from Settings.
      startActivityForResultWith<RickyBadgesActivity>([this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    if (action == RickyHomeUi::TRANSFER) {
      activityManager.goToFileTransfer();
      return;
    }
    if (action == RickyHomeUi::WEREAD) {
      activityManager.goToWeRead();
      return;
    }
    if (action == RickyHomeUi::FILES) {
      activityManager.goToFileBrowser();
      return;
    }
    if (books && action >= 0 && action < static_cast<int>(books->size())) {
      selected = action;
      openSelected();
      return;
    }
  }
  if (!books || books->empty()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }

  int x = 0;
  int y = 0;
  if (rickyHome) {
    // Already routed above, including the profile on an empty home.
  } else {
    if (mappedInput.wasScreenTouchDown(x, y)) {
      const int touched = indexFromPoint(x, y);
      if (touched >= 0 && touched != selected) {
        selected = touched;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasScreenTapped(x, y)) {
      const int touched = indexFromPoint(x, y);
      if (touched >= 0) {
        selected = touched;
        openSelected();
      }
      return;
    }
  }

  const int count = static_cast<int>(books->size());
  const auto swipe = mappedInput.wasSwipe();
  if (rickyHome && swipe != MappedInputManager::SwipeDir::None) {
    // A page holds RickyHomeLayout::PAGE_BOOKS books; moving the selection by one book
    // changed the page only every third swipe, so most swipes did nothing visible.
    const int page = RickyHomeLayout::PAGE_BOOKS;
    const int pages = (count + page - 1) / page;
    const int current = RickyHomeLayout::pageStart(selected) / page;
    const bool forward = swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up;
    selected = ((current + (forward ? 1 : pages - 1)) % pages) * page;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) || swipe == MappedInputManager::SwipeDir::Up ||
      swipe == MappedInputManager::SwipeDir::Left) {
    selected = (selected + 1) % count;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) || swipe == MappedInputManager::SwipeDir::Down ||
      swipe == MappedInputManager::SwipeDir::Right) {
    selected = (selected + count - 1) % count;
    requestUpdate();
    return;
  }
}

void InxRecentActivity::drawFlow(const Rect& content) {
  const bool showSelection = showMainTabContentSelection();
  const int count = static_cast<int>(books->size());
  const RecentBook& book = (*books)[selected];
  const int carouselHeight = std::max(1, content.height * 50 / 100);
  const Rect carousel{content.x, content.y + 5, content.width, carouselHeight};
  drawSparseInk(renderer, carousel);

  const auto centerSize = InxCoverGeometry::fit(carousel.width, std::max(1, carousel.height * 94 / 100));
  const Rect center{carousel.x + (carousel.width - centerSize.width) / 2,
                    carousel.y + (carousel.height - centerSize.height) / 2, centerSize.width, centerSize.height};
  setThumbnailHeight(center.height);
  const auto sideSize = InxCoverGeometry::fit(carousel.width, std::max(1, center.height * 90 / 100));
  const int sideTop = center.y + (center.height - sideSize.height) / 2;
  const int sideGap = std::max(kGap, content.width * 4 / 100);
  // The render scope clips side covers and all following text to the content area.
  if (selected > 0) {
    drawBookCover(selected - 1, Rect{center.x - sideSize.width - sideGap, sideTop, sideSize.width, sideSize.height});
  }
  if (selected + 1 < count) {
    drawBookCover(selected + 1, Rect{center.x + center.width + sideGap, sideTop, sideSize.width, sideSize.height});
  }
  drawBookCover(selected, center);
  if (showSelection) drawThickFrame(renderer, center);

  const int dividerY = carousel.y + carousel.height + 10;
  renderer.drawLine(content.x, dividerY, content.x + content.width - 1, dividerY, true);
  const int textY = dividerY + 15;
  const int textX = content.x + kPagePadding;
  drawBookText(renderer, book, textX, textY, content.width - kPagePadding * 2, true);

  const ReadingBookStats* stats = statsAt(selected);
  const uint8_t progress = progressOf(stats);
  const int progressY = textY + (UiHighDpiProfile::enabled ? renderer.getLineHeight(UI_10_FONT_ID) +
                                                                 renderer.getLineHeight(SMALL_FONT_ID) + 24
                                                           : 58);
  const int progressWidth = std::max(24, (content.width - kPagePadding * 2) / 2);
  drawMiniProgress(renderer, Rect{textX, progressY, progressWidth, kProgressHeight}, progress);
  char percent[8];
  snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(progress));
  renderer.drawText(SMALL_FONT_ID, textX + progressWidth + 12,
                    progressY - (renderer.getLineHeight(SMALL_FONT_ID) - kProgressHeight) / 2, percent);

  const int metricsTop = progressY + (UiHighDpiProfile::enabled ? 48 : 34);
  const int metricWidth = (content.width - kPagePadding * 2 - kGap) / 2;
  const int metricHeight = std::max(1, (content.y + content.height - metricsTop) / 2);
  const std::string total = stats ? ReadingStatsAnalytics::formatDurationHm(stats->totalReadingMs) : "0m";
  const std::string last = stats ? ReadingStatsAnalytics::formatDurationHm(stats->lastSessionMs) : "0m";
  char sessions[16];
  char chapter[8];
  snprintf(sessions, sizeof(sessions), "%u", stats ? static_cast<unsigned>(stats->sessions) : 0U);
  snprintf(chapter, sizeof(chapter), "%u%%", stats ? static_cast<unsigned>(stats->chapterProgressPercent) : 0U);
  drawMetric(renderer, textX, metricsTop, total.c_str(), tr(STR_TOTAL_TIME), metricWidth);
  drawMetric(renderer, textX + metricWidth + kGap, metricsTop, sessions, tr(STR_SESSIONS), metricWidth);
  drawMetric(renderer, textX, metricsTop + metricHeight, last.c_str(), tr(STR_LAST_SESSION), metricWidth);
  drawMetric(renderer, textX + metricWidth + kGap, metricsTop + metricHeight, chapter, tr(STR_CHAPTER_PROGRESS),
             metricWidth);
}

void InxRecentActivity::drawGrid(const Rect& content) {
  const bool showSelection = showMainTabContentSelection();
  const int start = InxRecentGeometry::pageStart(selected, static_cast<int>(books->size()), layout());
  const int cellWidth = content.width / 2;
  const int cellHeight = content.height / 2;
  for (int slot = 0; slot < 4 && start + slot < static_cast<int>(books->size()); ++slot) {
    const int index = start + slot;
    const int column = slot % 2;
    const int row = slot / 2;
    const Rect cell{content.x + column * cellWidth + kGap / 2, content.y + row * cellHeight + kGap / 2,
                    cellWidth - kGap, cellHeight - kGap};
    if (showSelection && index == selected) drawSparseInk(renderer, cell);
    const Rect cover = fitCoverRect(Rect{cell.x + kGap, cell.y + kGap, cell.width - kGap * 2, cell.height - kGap * 2});
    if (slot == 0) setThumbnailHeight(cover.height);
    drawBookCover(index, cover);
    if (showSelection && index == selected) drawThickFrame(renderer, cover);
    const int barWidth = std::max(24, cover.width - 30);
    const int barX = cover.x + (cover.width - barWidth) / 2;
    const int barY = cover.y + cover.height - 18;
    renderer.fillRect(barX - 2, barY - 2, barWidth + 4, kProgressHeight + 4, false);
    drawMiniProgress(renderer, Rect{barX, barY, barWidth, kProgressHeight}, progressOf(statsAt(index)));
  }
}

void InxRecentActivity::drawList(const Rect& content) {
  const bool showSelection = showMainTabContentSelection();
  const int start = InxRecentGeometry::pageStart(selected, static_cast<int>(books->size()), layout());
  const int rowHeight = content.height / 5;
  for (int slot = 0; slot < 5 && start + slot < static_cast<int>(books->size()); ++slot) {
    const int index = start + slot;
    const Rect row{content.x, content.y + slot * rowHeight, content.width, rowHeight};
    if (showSelection && index == selected) drawSparseInk(renderer, row);
    const Rect cover = fitCoverRect(Rect{row.x + kPagePadding, row.y + 5, 88, row.height - 10});
    if (slot == 0) setThumbnailHeight(cover.height);
    drawBookCover(index, cover);
    const int textX = cover.x + cover.width + 14;
    const int textWidth = row.x + row.width - kPagePadding - textX;
    drawBookText(renderer, (*books)[index], textX, row.y + 14, textWidth, true);
    drawMiniProgress(renderer,
                     Rect{textX, row.y + row.height - 15, std::max(24, textWidth * 80 / 100), kProgressHeight},
                     progressOf(statsAt(index)));
    if (slot + 1 < 5 && start + slot + 1 < static_cast<int>(books->size())) {
      drawDottedSeparator(renderer, row.x + kGap, row.y + row.height - 1, row.width - kGap * 2);
    }
  }
}

void InxRecentActivity::drawIcons(const Rect& content) {
  const bool showSelection = showMainTabContentSelection();
  const int start = InxRecentGeometry::pageStart(selected, static_cast<int>(books->size()), layout());
  const int cellWidth = content.width / 3;
  const int cellHeight = content.height / 3;
  for (int slot = 0; slot < 9 && start + slot < static_cast<int>(books->size()); ++slot) {
    const int index = start + slot;
    const int column = slot % 3;
    const int row = slot / 3;
    const Rect cell{content.x + column * cellWidth + 5, content.y + row * cellHeight + 5, cellWidth - 10,
                    cellHeight - 10};
    const Rect cover = fitCoverRect(Rect{cell.x + 4, cell.y + 4, cell.width - 8, cell.height - 8});
    if (slot == 0) setThumbnailHeight(cover.height);
    drawBookCover(index, cover);
    drawProgressBadge(renderer, cover, progressOf(statsAt(index)));
    if (showSelection && index == selected)
      drawThickFrame(renderer, Rect{cover.x - 2, cover.y - 2, cover.width + 4, cover.height + 4});
  }
}

void InxRecentActivity::drawCover(const Rect& content) {
  const bool showSelection = showMainTabContentSelection();
  constexpr int progressGap = 10;
  const int progressBlockHeight = progressGap + 8;
  const int targetWidth = std::max(1, content.width * 78 / 100);
  const Rect cover = fitCoverRect(Rect{content.x + (content.width - targetWidth) / 2, content.y + 6, targetWidth,
                                       std::max(1, content.height - progressBlockHeight - 12)});
  setThumbnailHeight(cover.height);
  drawBookCover(selected, cover);
  if (showSelection) drawThickFrame(renderer, cover);
  const int barWidth = std::max(24, cover.width * 80 / 100);
  drawMiniProgress(renderer,
                   Rect{cover.x + (cover.width - barWidth) / 2, cover.y + cover.height + progressGap, barWidth, 8},
                   progressOf(statsAt(selected)));
}

void InxRecentActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  drawPageHeader(Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));
  const Rect content = contentRect();

  if (content.width > 0 && content.height > 0) {
    const GfxRenderer::ClipScope clip(renderer, content.x, content.y, content.width, content.height);
    if (rickyHome) {
      const int start = RickyHomeLayout::pageStart(selected);
      const int inset = ricky::ds::space::kPageMargin;
      const Rect bounds{content.x + inset, content.y, std::max(1, content.width - inset * 2), content.height};
      rickyHome->configure(bounds, selected, showMainTabContentSelection(), progressOf(statsAt(start)));
      rickyHome->renderUi();
    } else if (!books || books->empty()) {
      UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_NO_RECENT_BOOKS), 2);
    } else {
      switch (layout()) {
        case InxRecentLayout::Flow:
          drawFlow(content);
          break;
        case InxRecentLayout::Grid:
          drawGrid(content);
          break;
        case InxRecentLayout::List:
          drawList(content);
          break;
        case InxRecentLayout::Icons:
          drawIcons(content);
          break;
        case InxRecentLayout::Cover:
          drawCover(content);
          break;
        case InxRecentLayout::Count:
          break;
      }
    }
  }

  const auto labels = mainTabButtonLabels(SETTINGS.standbyShortcutEnabled && !BoardConfig::hasHomeKey() ? tr(STR_STANDBY_TITLE) : "", tr(STR_OPEN),
                                          books && books->size() > 1, false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (usesMainTabBar() && mappedInput.hasTouch() && !mainTabsAtBottom()) {
    int top, right, bottom, left;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int footerTop = renderer.getScreenHeight() - InxRecentGeometry::footerReservedHeight;
    GUI.drawMainTabStatusBar(renderer, Rect{left, footerTop, std::max(0, width - left - right),
                                            std::max(0, renderer.getScreenHeight() - bottom - footerTop)});
  } else if (!hasMainTabStatusBar()) {
    Rect battery = InxRecentGeometry::batteryRect(safeArea);
    if (mainTabsAtBottom()) battery.y = safeArea.y + metrics.topPadding + 5;
    GUI.drawBatteryRight(renderer, battery,
                         SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);
  }
  renderer.displayBuffer();
  coverWorkPending = true;
}
