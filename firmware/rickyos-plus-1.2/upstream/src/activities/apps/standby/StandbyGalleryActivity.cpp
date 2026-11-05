#include "StandbyGalleryActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "RickyWallpaperDownloadActivity.h"
#include "activities/ActivityManager.h"
#include "activities/util/ImageViewerActivity.h"
#include "components/RickyDesignUi.h"
#include "components/RickyGrayStroke.h"
#include "components/UITheme.h"
#include "util/PanelTileCache.h"

namespace fui = freeink::ui;
namespace ds = ricky::ds;

namespace {
template <typename Screen>
void contentMargins(Screen& screen, const Rect& bounds, int height) {
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0, static_cast<int16_t>(height - bounds.y - bounds.height), 0});
}

constexpr int kColumns = 3;
// The plate around a tile: the inverted selection shows as this ink margin.
constexpr int kPlate = 8;
// Thumbnails are tracked in a 32-bit mask per page.
constexpr int kMaxPageTiles = 32;

constexpr StrId kFilterLabels[] = {StrId::STR_RICKY_BOOKS_ALL, StrId::STR_RICKY_DEFAULT_PICTURES,
                                   StrId::STR_RICKY_MY_PICTURES};

fui::Rect inset(const fui::Rect& r, const int by) {
  return {static_cast<int16_t>(r.x + by), static_cast<int16_t>(r.y + by), static_cast<int16_t>(r.width - 2 * by),
          static_cast<int16_t>(r.height - 2 * by)};
}

// A pill or a button: ink when chosen, else paper with the light-gray hairline edge the
// Home and Library controls rest with.
void control(const GfxRenderer& renderer, const fui::Rect& r, const bool chosen, const int radius) {
  RickyDesignUi::fill(renderer, r, chosen ? ds::gray::kInk : ds::gray::kPaper, radius);
  if (!chosen) RickyGrayStroke::ring(renderer, r.x, r.y, r.width, r.height, ds::stroke::kHairline, radius, ds::gray::kLine);
}

// One line centred in a control: RickyDesignUi::text starts at the top of its box.
void label(const GfxRenderer& renderer, const fui::Rect& r, const char* value, const int font, const bool inverted) {
  const int height = renderer.getLineHeight(RickyDesignUi::fontFor(renderer, font, value));
  RickyDesignUi::text(renderer,
                      fui::Rect{r.x, static_cast<int16_t>(r.y + (r.height - height) / 2), r.width,
                                static_cast<int16_t>(height)},
                      value, font, inverted, fui::TextAlign::Center);
}

void pageArrow(fui::DrawTarget& target, const fui::Rect& r, const bool left, const bool enabled) {
  const auto paint = fui::Paint::solid(enabled ? fui::Color::Black : fui::Color::LightGray);
  const int16_t midX = static_cast<int16_t>(r.x + r.width / 2);
  const int16_t midY = static_cast<int16_t>(r.y + r.height / 2);
  const int16_t tip = static_cast<int16_t>(left ? midX - 5 : midX + 5);
  const int16_t tail = static_cast<int16_t>(left ? midX + 4 : midX - 4);
  target.line({tail, static_cast<int16_t>(midY - 9)}, {tip, midY}, 2, paint);
  target.line({tip, midY}, {tail, static_cast<int16_t>(midY + 9)}, 2, paint);
}
}  // namespace

const char* StandbyGalleryActivity::headerTitle() const {
  if (mode == Mode::Browse) return tr(STR_GALLERY_TITLE);
  if (target == StandbyPictures::Target::Reading) return tr(STR_RICKY_SELECT_READING_BACKGROUND);
  if (target == StandbyPictures::Target::Sticker) return tr(STR_RICKY_SELECT_TRANSPARENT);
  return target == StandbyPictures::Target::PowerOff ? tr(STR_RICKY_SELECT_POWER_OFF_WALLPAPER)
                                                     : tr(STR_RICKY_SELECT_WALLPAPER);
}

void StandbyGalleryActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(
      ACTION_TOOL,
      [](const fui::ActionEvent& event, void* user) { static_cast<StandbyGalleryActivity*>(user)->runTool(event.value); },
      this);
  reload();
  // Open on the picture in use, so the reader sees where they are.
  const auto current = std::find_if(pictures.begin(), pictures.end(),
                                    [this](const StandbyPictures::Entry& entry) { return entry.path == inUse; });
  nav.selected = current == pictures.end() ? 0 : static_cast<int>(current - pictures.begin());
}

void StandbyGalleryActivity::onExit() {
  pictures.clear();
  pictures.shrink_to_fit();
  UiListActivity::onExit();
}

void StandbyGalleryActivity::reload() {
  RenderLock lock(*this);
  listFailed = target == StandbyPictures::Target::Sticker ? !StandbyPictures::listStickers(filter, pictures)
                                                          : !StandbyPictures::list(filter, pictures);
  inUse = StandbyPictures::sourceInUse(target);
  thumbPageStart = -1;
  thumbPending = 0;
  thumbAttempted = 0;
  requestUpdate();
}

void StandbyGalleryActivity::runTool(const int tool) {
  switch (tool) {
    case FilterAll:
    case FilterDefaults:
    case FilterMine: {
      const auto chosen = static_cast<StandbyPictures::Filter>(tool - FilterAll);
      if (chosen == filter) break;
      filter = chosen;
      reload();
      nav.selected = 0;
      break;
    }
    case PrevPage:
      turnPage(-1);
      break;
    case NextPage:
      turnPage(1);
      break;
    case Download:
      app.clearTapFlash();
      // Restarts to Home when it leaves after using the network, like the font downloader.
      startActivityForResultWith<RickyWallpaperDownloadActivity>([this](const ActivityResult&) { reload(); });
      return;
    case Transfer:
      app.clearTapFlash();
      activityManager.goToFileTransfer();
      return;
    default:
      return;
  }
  app.clearTapFlash();
  requestUpdate();
}

void StandbyGalleryActivity::turnPage(const int direction) {
  RenderLock lock(*this);
  const int count = listCount();
  if (count == 0) return;
  const int capacity = std::max(1, pageCapacity);
  const int start = std::clamp<int>(nav.selected, 0, count - 1) / capacity * capacity + direction * capacity;
  if (start < 0 || start >= count) return;
  nav.selected = start;
  requestUpdate();
}

bool StandbyGalleryActivity::handleCustomInput() {
  // A swipe turns the page; the grid has no viewport to scroll.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    turnPage(1);
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    turnPage(-1);
    return true;
  }
  return false;
}

void StandbyGalleryActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  std::vector<std::string> paths;
  paths.reserve(pictures.size());
  for (const auto& entry : pictures) paths.push_back(entry.path);
  const auto previewTarget = target == StandbyPictures::Target::PowerOff ? ImageViewerActivity::WallpaperTarget::PowerOff
                             : target == StandbyPictures::Target::Reading
                                 ? ImageViewerActivity::WallpaperTarget::Reading
                             : target == StandbyPictures::Target::Sticker
                                 ? ImageViewerActivity::WallpaperTarget::Sticker
                                 : ImageViewerActivity::WallpaperTarget::Standby;
  const bool opened = startActivityForResultWith<ImageViewerActivity>(
      [this](const ActivityResult& result) {
        // Back on the picture last shown, whether or not it was set.
        if (const auto* entry = std::get_if<FilePathResult>(&result.data)) {
          for (size_t i = 0; i < pictures.size(); ++i)
            if (pictures[i].path == entry->path) nav.selected = static_cast<int>(i);
        }
        inUse = StandbyPictures::sourceInUse(target);
        requestUpdate();
      },
      std::move(paths), index, previewTarget);
  if (!opened) LOG_ERR("STANDBY", "Cannot open the picture preview");
}

void StandbyGalleryActivity::loop() {
  UiListActivity::loop();
  if (thumbPending) {
    makePendingThumbnail();
  } else {
    prefetchThumbnail();
  }
}

void StandbyGalleryActivity::prefetchThumbnail() {
  // Quiet for a moment after the page completed, so a reader still tapping is not
  // held up by a decode they did not ask for.
  constexpr unsigned long kSettleMs = 1500;
  StandbyPictures::Entry entry;
  int width = 0;
  int height = 0;
  {
    RenderLock lock(*this);
    if (pageSettledMs == 0 || millis() - pageSettledMs < kSettleMs || thumbWidth <= 0) return;
    const int nextStart = thumbPageStart + std::max(1, pageCapacity);
    const int nextEnd = std::min(listCount(), nextStart + std::max(1, pageCapacity));
    if (prefetchIndex < nextStart) prefetchIndex = nextStart;
    if (prefetchIndex >= nextEnd) return;
    entry = pictures[static_cast<size_t>(prefetchIndex++)];
    width = thumbWidth;
    height = thumbHeight;
  }
  if (Storage.exists(StandbyPictures::thumbnailPath(entry, width, height).c_str())) return;
  const unsigned long started = millis();
  bool made = false;
  {
    RenderLock lock(*this);
    GfxRenderer::FrameBufferLoan loan(renderer);
    made = StandbyPictures::ensureThumbnail(entry, width, height);
  }
  LOG_INF("STANDBY", "prefetched thumbnail %s in %lu ms: %s", made ? "ready" : "failed", millis() - started,
          entry.path.c_str());
  // The decode borrowed the framebuffer: rebuild it. The page itself is unchanged, so
  // the panel is not driven.
  RenderLock lock(*this);
  requestUpdate();
}

void StandbyGalleryActivity::makePendingThumbnail() {
  StandbyPictures::Entry entry;
  int width = 0;
  int height = 0;
  {
    RenderLock lock(*this);
    if (!thumbPending) return;
    const int slot = __builtin_ctz(thumbPending);
    thumbPending &= thumbPending - 1;
    thumbAttempted |= 1u << slot;
    const int index = thumbPageStart + slot;
    if (index < 0 || index >= listCount()) return;
    entry = pictures[static_cast<size_t>(index)];
    width = thumbWidth;
    height = thumbHeight;
  }
  // One picture per loop pass: input queued meanwhile is handled before the next. The
  // decode borrows the framebuffer, which the repaint after each one redraws, so the
  // page fills in picture by picture instead of all at once at the end.
  const unsigned long started = millis();
  bool made = false;
  {
    RenderLock lock(*this);
    GfxRenderer::FrameBufferLoan loan(renderer);
    made = StandbyPictures::ensureThumbnail(entry, width, height);
  }
  LOG_INF("STANDBY", "thumbnail %s in %lu ms: %s", made ? "ready" : "failed", millis() - started, entry.path.c_str());
  RenderLock lock(*this);
  requestUpdate();
}

void StandbyGalleryActivity::buildScreen(UiScreen& screen) {
  contentMargins(screen, pageContentRect(), renderer.getScreenHeight());
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.insetContent(fui::Insets{static_cast<int16_t>(theme.spaceLg), theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = ds::space::kGutter;
  const int compact = ds::layout::kButtonCompactHeight;
  const int captionHeight = target.lineHeight(theme.smallText.font);

  // Filters along the top.
  const auto tools = screen.takeTop(static_cast<int16_t>(compact), static_cast<int16_t>(theme.spaceLg));
  int chipX = tools.x;
  for (int i = 0; i < static_cast<int>(std::size(kFilterLabels)); ++i) {
    const char* text = I18N.get(kFilterLabels[i]);
    const int textWidth = renderer.getTextWidth(RickyDesignUi::fontFor(renderer, RickyDesignUi::CAPTION, text), text);
    const fui::Rect chip{static_cast<int16_t>(chipX), tools.y, static_cast<int16_t>(textWidth + 2 * 24),
                         static_cast<int16_t>(compact)};
    const bool chosen = static_cast<int>(filter) == i;
    control(renderer, chip, chosen, compact / 2);
    label(renderer, chip, text, RickyDesignUi::CAPTION, chosen);
    screen.frame().hit(chip, ACTION_TOOL, FilterAll + i, fui::InputTouch);
    chipX += chip.width + 12;
  }

  // Download and transfer at the bottom, the page readout above them.
  const auto buttons = screen.takeBottom(static_cast<int16_t>(ds::layout::kButtonHeight), static_cast<int16_t>(gap));
  const int buttonWidth = (buttons.width - gap) / 2;
  const fui::Rect download{buttons.x, buttons.y, static_cast<int16_t>(buttonWidth), buttons.height};
  const fui::Rect transfer{static_cast<int16_t>(buttons.right() - buttonWidth), buttons.y,
                           static_cast<int16_t>(buttonWidth), buttons.height};
  control(renderer, download, false, ds::radius::kControl);
  control(renderer, transfer, false, ds::radius::kControl);
  label(renderer, download, tr(STR_RICKY_WALLPAPER_DOWNLOAD), RickyDesignUi::BODY, false);
  label(renderer, transfer, tr(STR_RICKY_TRANSFER_PICTURES), RickyDesignUi::BODY, false);
  screen.frame().hit(download, ACTION_TOOL, Download, fui::InputTouch);
  screen.frame().hit(transfer, ACTION_TOOL, Transfer, fui::InputTouch);
  const auto pager = screen.takeBottom(static_cast<int16_t>(compact), static_cast<int16_t>(gap));

  // The grid: tiles shaped like the screen, as many rows as fit.
  const auto grid = screen.body();
  const int count = listCount();
  const int cellWidth = (grid.width - gap * (kColumns - 1)) / kColumns;
  const int innerWidth = cellWidth - 2 * kPlate;
  const int innerHeight = innerWidth * renderer.getScreenHeight() / std::max(1, renderer.getScreenWidth());
  const int cellHeight = innerHeight + 2 * kPlate;
  const int rows = std::max(1, (grid.height + gap) / (cellHeight + gap));
  const int capacity = std::min(kMaxPageTiles, rows * kColumns);
  const int selected = count == 0 ? 0 : std::clamp<int>(nav.selected, 0, count - 1);
  nav.selected = selected;
  const int start = selected / capacity * capacity;
  const int visible = std::min(capacity, count - start);
  pageCapacity = capacity;
  if (start != thumbPageStart || innerWidth != thumbWidth || innerHeight != thumbHeight) {
    thumbPageStart = start;
    thumbAttempted = 0;
    prefetchIndex = -1;
  }
  thumbWidth = innerWidth;
  thumbHeight = innerHeight;
  thumbPending = 0;

  if (count == 0) {
    const char* message = listFailed ? tr(STR_FAILED_LOWER) : tr(STR_RICKY_STANDBY_EMPTY);
    RickyDesignUi::secondaryText(renderer,
                                 fui::Rect{grid.x, static_cast<int16_t>(grid.y + grid.height / 2 - captionHeight),
                                           grid.width, static_cast<int16_t>(captionHeight * 2)},
                                 message, RickyDesignUi::BODY, false, fui::TextAlign::Center);
  }

  const bool focus = showMainTabContentSelection();
  for (int slot = 0; slot < visible; ++slot) {
    const int index = start + slot;
    const auto& entry = pictures[static_cast<size_t>(index)];
    const fui::Rect cell{static_cast<int16_t>(grid.x + slot % kColumns * (cellWidth + gap)),
                         static_cast<int16_t>(grid.y + slot / kColumns * (cellHeight + gap)),
                         static_cast<int16_t>(cellWidth), static_cast<int16_t>(cellHeight)};
    const bool picked = focus && index == selected;
    if (picked) RickyDesignUi::fill(renderer, cell, ds::gray::kInk, ds::radius::kControl);
    const fui::Rect art = inset(cell, kPlate);
    const std::string thumb = StandbyPictures::thumbnailPath(entry, innerWidth, innerHeight);
    char key[48];
    snprintf(key, sizeof(key), "|sg|%d,%d,%d,%d|%d", art.x, art.y, art.width, art.height, picked ? 1 : 0);
    const bool painted = PanelTileCache::draw(renderer, thumb + key, thumb, art.x, art.y, art.width, art.height, [&] {
      HalFile file;
      if (!Storage.openFileForRead("STANDBY", thumb.c_str(), file)) return false;
      Bitmap bitmap(file);
      return bitmap.parseHeaders() == BmpReaderError::Ok &&
             GUI.drawCoverThumbFill(renderer, bitmap, Rect{art.x, art.y, art.width, art.height});
    });
    if (!painted) {
      RickyDesignUi::fill(renderer, art, ds::gray::kTint);
      if (!(thumbAttempted & (1u << slot))) thumbPending |= 1u << slot;
    }
    // A picture at rest gets the light-gray cover edge, as a book on the shelf does.
    if (!picked) RickyGrayStroke::ring(renderer, art.x, art.y, art.width, art.height, ds::stroke::kCoverEdge, 0, ds::gray::kLine);
    if (entry.path == inUse) {
      // "In use" sits on the picture's lower left, paper on whatever the picture is.
      const char* text = tr(STR_RICKY_IN_USE);
      const int textWidth = renderer.getTextWidth(RickyDesignUi::fontFor(renderer, RickyDesignUi::CAPTION, text), text);
      const int badgeHeight = captionHeight + 16;
      const fui::Rect badge{static_cast<int16_t>(art.x + 12),
                            static_cast<int16_t>(art.bottom() - 12 - badgeHeight),
                            static_cast<int16_t>(std::min<int>(textWidth + 32, art.width - 24)),
                            static_cast<int16_t>(badgeHeight)};
      control(renderer, badge, false, badgeHeight / 2);
      label(renderer, badge, text, RickyDesignUi::CAPTION, false);
    }
    screen.frame().hit(cell, ACTION_ROW, index, fui::InputTouch);
  }
  nav.drawnCount = count;
  nav.onListRendered(start, visible, true);
  if (thumbPending) {
    pageSettledMs = 0;
  } else if (pageSettledMs == 0) {
    pageSettledMs = millis();
  }

  // Page readout between the two page buttons.
  const int pages = std::max(1, (count + capacity - 1) / capacity);
  const int page = start / capacity + 1;
  snprintf(pageText, sizeof(pageText), "%d / %d", page, pages);
  const int arrowWidth = ds::layout::kTouchMin;
  const fui::Rect prev{pager.x, pager.y, static_cast<int16_t>(arrowWidth), pager.height};
  const fui::Rect next{static_cast<int16_t>(pager.right() - arrowWidth), pager.y, static_cast<int16_t>(arrowWidth),
                       pager.height};
  const bool hasPrev = page > 1;
  const bool hasNext = page < pages;
  // Only a page button that goes somewhere is drawn; the readout stays centred.
  if (hasPrev) {
    control(renderer, prev, false, ds::radius::kControl);
    pageArrow(target, prev, true, true);
  }
  if (hasNext) {
    control(renderer, next, false, ds::radius::kControl);
    pageArrow(target, next, false, true);
  }
  if (pages > 1)
    label(renderer,
          fui::Rect{static_cast<int16_t>(prev.right()), pager.y, static_cast<int16_t>(next.x - prev.right()),
                    pager.height},
          pageText, RickyDesignUi::BODY, false);
  if (hasPrev) screen.frame().hit(prev, ACTION_TOOL, PrevPage, fui::InputTouch);
  if (hasNext) screen.frame().hit(next, ACTION_TOOL, NextPage, fui::InputTouch);
}
