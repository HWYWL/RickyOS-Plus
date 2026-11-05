#pragma once

#include <BoardConfig.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "InxRecentLayout.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"

#include "Memory.h"
#include "components/RickyHomeUi.h"

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
#include <cstddef>

#include "Memory.h"
#endif

struct ReadingBookStats;
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
class HalFile;
#endif

class InxRecentActivity final : public Activity {
  enum class CoverCacheState : uint8_t { Unchecked, Ready, Missing, Unavailable };
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  enum class CoverCacheLoadResult : uint8_t { Loaded, Stream, Invalid };

  struct CoverRamCache {
    memory::ByteBuffer bytes;
    size_t size = 0;
    bool attempted = false;
  };

  static constexpr size_t MAX_CACHED_COVER_FILE_BYTES = 64 * 1024;
  static constexpr size_t MAX_COVER_CACHE_BYTES = 512 * 1024;
  static constexpr size_t COVER_CACHE_MAX_ALLOC_RESERVE = 8 * 1024;
#endif

  const std::vector<RecentBook>* books = nullptr;
  std::array<const ReadingBookStats*, RecentBooksStore::MAX_RECENT_BOOKS> bookStats{};
  std::array<CoverCacheState, RecentBooksStore::MAX_RECENT_BOOKS> targetCoverStates{};
  std::array<CoverCacheState, RecentBooksStore::MAX_RECENT_BOOKS> fallbackCoverStates{};
  // Thumbnail height already prepared per book during this visit: one attempt each, so a
  // cover that still cannot be drawn afterwards never loops prepare -> repaint.
  std::array<int16_t, RecentBooksStore::MAX_RECENT_BOOKS> preparedHeights{};
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  std::array<CoverRamCache, RecentBooksStore::MAX_RECENT_BOOKS> targetCoverCaches{};
  std::array<CoverRamCache, RecentBooksStore::MAX_RECENT_BOOKS> fallbackCoverCaches{};
  size_t cachedCoverBytes = 0;
#endif
  int selected = 0;
  int thumbnailHeight = 0;
  // Set by render() when a cover may still need a thumbnail; loop() prepares it
  // after that frame is on the panel instead of holding the frame back.
  std::atomic<bool> coverWorkPending{false};
  std::unique_ptr<RickyHomeUi> rickyHome;

  InxRecentLayout layout() const;
  Rect contentRect() const;
  const ReadingBookStats* statsAt(int index) const;
  int indexFromPoint(int x, int y) const;
  void openSelected();
  void setThumbnailHeight(int displayHeight);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
  void clearCoverCaches();
  CoverCacheLoadResult tryLoadCoverCache(HalFile& file, CoverRamCache& cache);
  bool tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state, CoverRamCache& cache);
#else
  bool tryDrawBookCover(const std::string& path, const Rect& bounds, CoverCacheState& state);
#endif
  bool drawBookCover(int bookIndex, const Rect& bounds);
  bool prepareNextMissingCover();

  void drawFlow(const Rect& content);
  void drawGrid(const Rect& content);
  void drawList(const Rect& content);
  void drawIcons(const Rect& content);
  void drawCover(const Rect& content);

 public:
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  // render() redraws the whole home screen from state; a newly prepared cover queues
  // its own update, which cancels the settle.
  bool wantsGraySettle() const override { return true; }
#endif
  explicit InxRecentActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InxRecent", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  MainTab mainTab() const override { return MainTab::Recent; }
  void selectMainTabContentEdge(MainTabContentEdge edge) override;
};
