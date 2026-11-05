#include "ImageViewerActivity.h"

#include "util/ReadingBackground.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/apps/standby/KeepPageFace.h"
#include "activities/apps/standby/StandbyPictures.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* IMAGE_PREVIEW_PATH = "/.crosspoint/image_preview.bmp";
constexpr const char* TRANSPARENT_PREVIEW_PATH = "/.crosspoint/image_preview.transparent.bmp";
constexpr const char* SLEEP_IMAGE_PATH = "/sleep.bmp";
constexpr const char* SLEEP_IMAGE_PART_PATH = "/sleep.bmp.part";
constexpr const char* SLEEP_IMAGE_BACKUP_PATH = "/sleep.bmp.bak";
constexpr const char* POWER_OFF_IMAGE_PATH = "/poweroff.bmp";
constexpr const char* POWER_OFF_IMAGE_PART_PATH = "/poweroff.bmp.part";
constexpr const char* POWER_OFF_IMAGE_BACKUP_PATH = "/poweroff.bmp.bak";
constexpr const char* READING_IMAGE_PART_PATH = "/readingbg.bmp.part";
constexpr const char* READING_IMAGE_BACKUP_PATH = "/readingbg.bmp.bak";
// Converted previews are kept: a PNG takes seconds to convert, and the same pictures
// are opened again and again while choosing a wallpaper. Bounded by kPreviewCacheMax.
constexpr const char* PREVIEW_CACHE_DIR = "/.crosspoint/previews";
constexpr int kPreviewCacheMax = 16;

std::string previewCachePath(const std::string& source) {
  HalFile file;
  uint32_t size = 0;
  if (Storage.openFileForRead("IMAGE", source.c_str(), file)) {
    size = static_cast<uint32_t>(file.fileSize());
    file.close();
  }
  // FNV-1a over path and size: a replaced picture under the same name converts again.
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](const uint8_t byte) { hash = (hash ^ byte) * 16777619u; };
  for (const char c : source) mix(static_cast<uint8_t>(c));
  for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>(size >> (i * 8)));
  char name[48];
  snprintf(name, sizeof(name), "%s/%08x.bmp", PREVIEW_CACHE_DIR, static_cast<unsigned>(hash));
  return name;
}

void prunePreviewCache() {
  HalFile dir = Storage.open(PREVIEW_CACHE_DIR);
  if (!dir || !dir.isDirectory()) return;
  std::vector<std::string> names;
  char name[64];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    entry.close();
    if (name[0] != '\0' && name[0] != '.') names.emplace_back(name);
  }
  dir.close();
  if (static_cast<int>(names.size()) < kPreviewCacheMax) return;
  // Simple and bounded: start the cache over rather than track ages.
  for (const auto& entry : names) Storage.remove((std::string(PREVIEW_CACHE_DIR) + "/" + entry).c_str());
}

constexpr StrId sleepCoverLabel(
    const bool sticker = false,
    const ImageViewerActivity::WallpaperTarget target = ImageViewerActivity::WallpaperTarget::Standby) {
  if (target == ImageViewerActivity::WallpaperTarget::PowerOff) return StrId::STR_RICKY_SET_POWER_OFF_WALLPAPER;
  if (target == ImageViewerActivity::WallpaperTarget::Reading) return StrId::STR_RICKY_SET_READING_BACKGROUND;
  return sticker ? StrId::STR_RICKY_SET_TRANSPARENT : StrId::STR_RICKY_SET_WALLPAPER;
}

Rect sleepCoverActionRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect(metrics.contentSidePadding,
              renderer.getScreenHeight() - metrics.contentSidePadding - metrics.menuRowHeight,
              renderer.getScreenWidth() - metrics.contentSidePadding * 2, metrics.menuRowHeight);
}

// The gallery's three buttons share the action row: power-off, reading background,
// then standby on the right where the single "set" button's thumb lands.
void galleryActionRects(const GfxRenderer& renderer, Rect& powerOff, Rect& reading, Rect& standby) {
  const Rect row = sleepCoverActionRect(renderer);
  const int gap = UITheme::getInstance().getMetrics().contentSidePadding / 2;
  const int width = (row.width - 2 * gap) / 3;
  powerOff = Rect(row.x, row.y, width, row.height);
  reading = Rect(row.x + width + gap, row.y, width, row.height);
  standby = Rect(row.x + row.width - width, row.y, width, row.height);
}

// The paper band the gallery's caption and buttons sit on, so the picture never shows
// through them; the rest of the screen stays the picture.
Rect galleryBand(const GfxRenderer& renderer) {
  const Rect row = sleepCoverActionRect(renderer);
  const int padding = UITheme::getInstance().getMetrics().contentSidePadding / 2;
  const int top = row.y - renderer.getLineHeight(UI_10_FONT_ID) - padding * 3;
  return Rect(0, top, renderer.getScreenWidth(), renderer.getScreenHeight() - top);
}

void galleryButton(const GfxRenderer& renderer, const Rect& r, const char* label, const bool primary) {
  const int radius = UITheme::getInstance().getMetrics().optionPopupSelectionRadius;
  renderer.fillRoundedRect(r.x, r.y, r.width, r.height, radius, primary ? Color::Black : Color::White);
  if (!primary) renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 2, radius, true);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, r.x + (r.width - textWidth) / 2,
                    r.y + (r.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label, !primary,
                    EpdFontFamily::BOLD);
}

StandbyPictures::Target pictureTarget(const ImageViewerActivity::WallpaperTarget target) {
  switch (target) {
    case ImageViewerActivity::WallpaperTarget::PowerOff:
      return StandbyPictures::Target::PowerOff;
    case ImageViewerActivity::WallpaperTarget::Reading:
      return StandbyPictures::Target::Reading;
    case ImageViewerActivity::WallpaperTarget::Sticker:
      return StandbyPictures::Target::Sticker;
    default:
      return StandbyPictures::Target::Standby;
  }
}
}  // namespace

ImageViewerActivity::ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                         const bool wallpaperPicker, const bool stickerPicker,
                                         const WallpaperTarget wallpaperTarget)
    : Activity("ImageViewer", renderer, mappedInput),
      filePath(std::move(path)),
      wallpaperPicker(wallpaperPicker),
      stickerPicker(stickerPicker),
      wallpaperTarget(wallpaperTarget) {}

ImageViewerActivity::ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::vector<std::string> pictures, const int index,
                                         const WallpaperTarget wallpaperTarget)
    : Activity("ImageViewer", renderer, mappedInput),
      wallpaperPicker(true),
      stickerPicker(wallpaperTarget == WallpaperTarget::Sticker),
      wallpaperTarget(wallpaperTarget),
      siblingImages(std::move(pictures)),
      gallery(true) {
  currentImageIndex = siblingImages.empty() ? -1 : std::clamp(index, 0, static_cast<int>(siblingImages.size()) - 1);
  if (currentImageIndex >= 0) filePath = siblingImages[static_cast<size_t>(currentImageIndex)];
}

void ImageViewerActivity::drawActions() const {
  if (!mappedInput.hasTouch()) return;
  if (!gallery) {
    GUI.drawActionButton(renderer, sleepCoverActionRect(renderer),
                         I18N.get(sleepCoverLabel(stickerPicker, wallpaperTarget)));
    return;
  }
  const Rect band = galleryBand(renderer);
  renderer.fillRect(band.x, band.y, band.width, band.height, false);
  renderer.fillRect(band.x, band.y, band.width, 2, true);
  // Which picture, and where it is in the gallery: "Leaves  3 / 12".
  std::string name = filePath.substr(filePath.find_last_of('/') + 1);
  if (const size_t dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) name.resize(dot);
  char position[24];
  snprintf(position, sizeof(position), "  %d / %d", currentImageIndex + 1, static_cast<int>(siblingImages.size()));
  name += position;
  const int padding = UITheme::getInstance().getMetrics().contentSidePadding / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, band.y + padding * 2, name.c_str());
  if (stickerPicker) {
    // A transparent wallpaper is only ever the standby overlay: one button.
    galleryButton(renderer, sleepCoverActionRect(renderer), tr(STR_RICKY_SET_TRANSPARENT), true);
    return;
  }
  Rect powerOff, reading, standby;
  galleryActionRects(renderer, powerOff, reading, standby);
  // The gallery's own screen is the primary button.
  galleryButton(renderer, powerOff, tr(STR_RICKY_SET_POWER_OFF_SHORT), wallpaperTarget == WallpaperTarget::PowerOff);
  galleryButton(renderer, reading, tr(STR_RICKY_SET_READING_SHORT), wallpaperTarget == WallpaperTarget::Reading);
  galleryButton(renderer, standby, tr(STR_RICKY_SET_STANDBY_SHORT), wallpaperTarget == WallpaperTarget::Standby);
}

bool ImageViewerActivity::tapAction() {
  if (!mappedInput.hasTouch()) return false;
  if (!gallery) {
    const Rect action = sleepCoverActionRect(renderer);
    if (!mappedInput.wasTapInRect(action.x, action.y, action.width, action.height)) return false;
    showSleepCoverOptions();
    return true;
  }
  if (stickerPicker) {
    const Rect action = sleepCoverActionRect(renderer);
    if (!mappedInput.wasTapInRect(action.x, action.y, action.width, action.height)) return false;
    setStickerFromGallery();
    return true;
  }
  Rect powerOff, reading, standby;
  galleryActionRects(renderer, powerOff, reading, standby);
  if (mappedInput.wasTapInRect(powerOff.x, powerOff.y, powerOff.width, powerOff.height)) {
    setFromGallery(WallpaperTarget::PowerOff);
    return true;
  }
  if (mappedInput.wasTapInRect(reading.x, reading.y, reading.width, reading.height)) {
    setFromGallery(WallpaperTarget::Reading);
    return true;
  }
  if (mappedInput.wasTapInRect(standby.x, standby.y, standby.width, standby.height)) {
    setFromGallery(WallpaperTarget::Standby);
    return true;
  }
  return false;
}

const char* ImageViewerActivity::wallpaperSource() const {
  return isPng() || isJpeg() ? previewPath.c_str() : filePath.c_str();
}

void ImageViewerActivity::rememberSource() const {
  StandbyPictures::rememberSource(pictureTarget(wallpaperTarget), filePath);
}

bool ImageViewerActivity::setFromGallery(const WallpaperTarget target) {
  // The gallery offers both screens; the install follows the one chosen. Back in the
  // gallery its own screen is marked again, so the switch is not undone here.
  wallpaperTarget = target;
  if (!doSetSleepCover(wallpaperSource(), false)) return false;
  ActivityResult result;
  result.data = FilePathResult{filePath};
  setResult(std::move(result));
  finish();
  return true;
}

bool ImageViewerActivity::setStickerFromGallery() {
  if (!doSetSticker()) return false;
  rememberSource();
  ActivityResult result;
  result.data = FilePathResult{filePath};
  setResult(std::move(result));
  finish();
  return true;
}

void ImageViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return;

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasImageExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
  }

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

namespace {
// The signature decides; the extension is only the fallback for a file too short to tell.
ImageViewerActivity::ImageKind sniffImageKind(const std::string& path) {
  using Kind = ImageViewerActivity::ImageKind;
  uint8_t head[8] = {};
  HalFile file;
  if (Storage.openFileForRead("IMAGE", path.c_str(), file) && file.read(head, sizeof(head)) == sizeof(head)) {
    static constexpr uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (memcmp(head, kPng, sizeof(kPng)) == 0) return Kind::Png;
    if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return Kind::Jpeg;
    if (head[0] == 'B' && head[1] == 'M') return Kind::Bmp;
  }
  if (FsHelpers::hasPngExtension(path)) return Kind::Png;
  if (FsHelpers::hasJpgExtension(path)) return Kind::Jpeg;
  return Kind::Bmp;
}
}  // namespace

bool ImageViewerActivity::isPng() const { return imageKind == ImageKind::Png; }
bool ImageViewerActivity::isJpeg() const { return imageKind == ImageKind::Jpeg; }

bool ImageViewerActivity::preparePreview() {
  previewPath = previewCachePath(filePath);
  if (Storage.exists(previewPath.c_str())) return true;
  if (!Storage.ensureDirectoryExists(PREVIEW_CACHE_DIR)) return false;
  prunePreviewCache();
  // Written aside and renamed, so a conversion cut short is never taken for a preview.
  const std::string outPath = previewPath + ".part";
  if (Storage.exists(outPath.c_str()) && !Storage.remove(outPath.c_str())) return false;

  bool prepared = false;
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    if (isPng()) {
      // Read Pico's panel shows 16 grays; keep them all instead of dithering to four.
      prepared = PngToBmpConverter::pngFileToGray8BmpFile(filePath.c_str(), outPath.c_str(), true);
    } else {
      HalFile input, output;
      if (Storage.openFileForRead("IMAGE", filePath.c_str(), input) &&
          Storage.openFileForWrite("IMAGE", outPath, output)) {
        // Read Pico's panel shows 16 grays; keep the photo's full gray range.
        prepared =
            JpegToBmpConverter::jpegFileToBmpStream(input, output, /*crop=*/false, JpegToBmpConverter::Output::Gray8);
        output.flush();
      }
    }
  }
  prepared = prepared && Storage.rename(outPath.c_str(), previewPath.c_str());
  if (!prepared) Storage.remove(outPath.c_str());
  return prepared;
}

void ImageViewerActivity::onEnter() {
  Activity::onEnter();
  imageReady = false;
  if (!wallpaperPicker && siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // No blank page first: the loading popup stays over the list until the picture lands
  // in one full refresh (a white screen for seconds read as a hang). The full GC16
  // commit below clears what was underneath.
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  imageKind = sniffImageKind(filePath);
  const bool needsPreview = isPng() || isJpeg();
  const unsigned long tPrep = millis();
  const bool prepared = !needsPreview || preparePreview();
  LOG_DBG("IMGV", "timing: preview %lu ms", millis() - tPrep);
  const char* bitmapPath = needsPreview ? previewPath.c_str() : filePath.c_str();
  HalFile file;
  // 1. Open the file
  if (!prepared) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_IMAGE_FILE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else if (Storage.openFileForRead("IMAGE", bitmapPath, file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), I18N.get(sleepCoverLabel(stickerPicker, wallpaperTarget)),
                                                (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      // The 16-level pass below draws the picture itself; a B/W pass first only cost time.
      const bool native16 = bitmap.hasGreyscale() && renderer.getGrayscaleLevels() == 16;
      renderer.clearScreen();
      if (!native16 && !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        return;
      }

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      drawActions();
      // Native 16-gray, as Standby and the sleep screen draw it. The overlay-mask path
      // also painted the Set button into its gray planes, turning it into a dark bar
      // with no readable label. Here the button is drawn in B/W and copied in.
      if (bitmap.hasGreyscale() && renderer.getGrayscaleLevels() == 16 && bitmap.rewindToData() == BmpReaderError::Ok) {
        const unsigned long tGray = millis();
        bool shown =
            renderer.beginGrayscale16() && renderer.drawBitmapGrayscale16(bitmap, x, y, pageWidth, pageHeight, 0, 0);
        LOG_DBG("IMGV", "timing: gray16 draw %lu ms", millis() - tGray);
        const unsigned long tCommit = millis();
#if !defined(SIMULATOR)
        // A clean full refresh for a picture that stays up, then the rails go off.
        if (shown) display.setNextGray16Profile(3);
#endif
        if (shown && mappedInput.hasTouch()) {
          const Rect action = gallery ? galleryBand(renderer) : sleepCoverActionRect(renderer);
          drawActions();
          renderer.copyBwToGrayscale16(action.x, action.y, action.width, action.height);
        }
        shown = shown && renderer.commitGrayscale16();
        LOG_DBG("IMGV", "timing: gray16 commit %lu ms", millis() - tCommit);
        renderer.cancelGrayscale16();
        if (!shown) {
          LOG_ERR("BMP", "16-gray preview failed");
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
        imageReady = shown;
        return;
      }
      if (bitmap.hasGreyscale()) {
        // Read Pico's bitmap renderer emits overlay masks, not UC8279 absolute planes.
        constexpr bool absolute = false;
        if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return;
        if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
        bool planesReady = true;
        for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
          if (bitmap.rewindToData() != BmpReaderError::Ok) {
            LOG_ERR("BMP", "Failed to rewind bitmap for grayscale rendering");
            planesReady = false;
            break;
          }
          renderer.clearScreen(absolute ? 0xFF : 0x00);
          renderer.setRenderMode(mode);
          if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
            planesReady = false;
            break;
          }
          GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
          drawActions();
          if (mode == GfxRenderer::GRAYSCALE_LSB) {
            renderer.copyGrayscaleLsbBuffers();
          } else {
            renderer.copyGrayscaleMsbBuffers();
          }
        }
        if (planesReady) renderer.displayGrayBuffer();

        // Rebuild the BW framebuffer for popups and subsequent differential updates.
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.clearScreen();
        if (bitmap.rewindToData() != BmpReaderError::Ok ||
            !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
          LOG_ERR("BMP", "Failed to rewind bitmap to restore the BW framebuffer");
          renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
          planesReady = false;
        }
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        renderer.cleanupGrayscaleWithFrameBuffer();
        if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        imageReady = planesReady;
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        imageReady = true;
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_IMAGE_FILE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void ImageViewerActivity::onExit() {
  Activity::onExit();
  imageReady = false;
  if (Storage.exists(TRANSPARENT_PREVIEW_PATH)) Storage.remove(TRANSPARENT_PREVIEW_PATH);
  renderer.clearScreen();
  // Leave no gray picture under the next page (see onEnter).
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

bool ImageViewerActivity::doSetSleepCover(const char* sourcePath, const bool transparent) {
  if (!imageReady) return false;
  const bool powerOff = wallpaperTarget == WallpaperTarget::PowerOff;
  const bool reading = wallpaperTarget == WallpaperTarget::Reading;
  if ((powerOff || reading) && transparent) return false;
  const char* destination = reading ? ReadingBackground::kPath : powerOff ? POWER_OFF_IMAGE_PATH : SLEEP_IMAGE_PATH;
  const char* part = reading ? READING_IMAGE_PART_PATH : powerOff ? POWER_OFF_IMAGE_PART_PATH : SLEEP_IMAGE_PART_PATH;
  const char* backup =
      reading ? READING_IMAGE_BACKUP_PATH : powerOff ? POWER_OFF_IMAGE_BACKUP_PATH : SLEEP_IMAGE_BACKUP_PATH;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const char* preparedPath = sourcePath;
  bool success = true;
  // Recover an interrupted rename before touching the last complete picture.
  if (Storage.exists(backup)) {
    success = Storage.exists(destination) ? Storage.remove(backup) : Storage.rename(backup, destination);
  }
  if (success && transparent) {
    GfxRenderer::FrameBufferLoan loan(renderer);
    success = PngToBmpConverter::pngFileToTransparentBmpFile(sourcePath, TRANSPARENT_PREVIEW_PATH, true);
    if (success) preparedPath = TRANSPARENT_PREVIEW_PATH;
  }

  success = success && (!Storage.exists(part) || Storage.remove(part));
  if (success) {
    HalFile inFile, outFile;
    if (Storage.openFileForRead("IMAGE", preparedPath, inFile) && Storage.openFileForWrite("IMAGE", part, outFile)) {
      const uint64_t expected = inFile.fileSize64();
      uint64_t copied = 0;
      // A full-screen 16-gray BMP is ~400 KB: 128-byte chunks took over ten seconds.
      static char buffer[4096];
      int bytesRead;
      while ((bytesRead = inFile.read(buffer, sizeof(buffer))) > 0) {
        if (outFile.write(buffer, bytesRead) != bytesRead) {
          success = false;
          break;
        }
        copied += bytesRead;
      }
      outFile.flush();
      success = success && bytesRead == 0 && expected > 0 && copied == expected && outFile.fileSize64() == expected;
    } else {
      success = false;
    }
  }

  if (success) {
    const bool hadPrevious = Storage.exists(destination);
    if ((hadPrevious && !Storage.rename(destination, backup)) || !Storage.rename(part, destination)) {
      if (hadPrevious && !Storage.exists(destination)) {
        Storage.rename(backup, destination);
      }
      success = false;
    }
  }

  if (success) {
    const uint8_t previousMode = SETTINGS.sleepScreen;
    const uint8_t previousFace = SETTINGS.rickyStandbyFace;
    const uint8_t previousPowerOff = SETTINGS.rickyPowerOffScreen;
    const uint8_t previousReading = SETTINGS.readingBackground;
    const uint8_t previousPaper = SETTINGS.readingPaper;
    if (reading) {
      // Choosing a picture shows it under the page, at medium unless a shade is set.
      SETTINGS.readingPaper = CrossPointSettings::READING_PAPER_PICTURE;
      if (SETTINGS.readingBackground == CrossPointSettings::READING_BACKGROUND_OFF)
        SETTINGS.readingBackground = CrossPointSettings::READING_BACKGROUND_MEDIUM;
    } else if (powerOff) {
      SETTINGS.rickyPowerOffScreen = CrossPointSettings::RICKY_POWER_OFF_CUSTOM;
    } else {
      SETTINGS.sleepScreen = transparent ? CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT
                                         : CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
      if (!transparent) SETTINGS.rickyStandbyFace = CrossPointSettings::RICKY_STANDBY_PICTURE;
    }
    if (!SETTINGS.saveToFile()) {
      SETTINGS.sleepScreen = previousMode;
      SETTINGS.rickyStandbyFace = previousFace;
      SETTINGS.rickyPowerOffScreen = previousPowerOff;
      SETTINGS.readingBackground = previousReading;
      SETTINGS.readingPaper = previousPaper;
      (void)SETTINGS.saveToFile();
      Storage.remove(destination);
      if (Storage.exists(backup)) {
        Storage.rename(backup, destination);
      }
      success = false;
    }
  }

  Storage.remove(part);
  if (success) Storage.remove(backup);
  if (success && reading) ReadingBackground::invalidate();
  // The gallery marks the picture each screen was set from.
  if (success && !transparent) rememberSource();
  GUI.drawPopup(renderer, success ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(1000);
  return success;
}

void ImageViewerActivity::showSleepCoverOptions() {
  if (!imageReady) return;
  if (gallery && stickerPicker) {
    setStickerFromGallery();
    return;
  }
  if (gallery) {
    // Keys only: the touch screen shows both buttons. Its own screen is listed first.
    static constexpr StrId standbyFirst[] = {StrId::STR_RICKY_SET_WALLPAPER, StrId::STR_RICKY_SET_POWER_OFF_WALLPAPER};
    static constexpr StrId powerOffFirst[] = {StrId::STR_RICKY_SET_POWER_OFF_WALLPAPER, StrId::STR_RICKY_SET_WALLPAPER};
    const bool offFirst = wallpaperTarget == WallpaperTarget::PowerOff;
    sleepCoverPopup.show(sleepCoverLabel(false, wallpaperTarget), offFirst ? powerOffFirst : standbyFirst, 2, 0,
                         [this, offFirst](const int index) {
                           const bool powerOff = (index == 0) == offFirst;
                           setFromGallery(powerOff ? WallpaperTarget::PowerOff : WallpaperTarget::Standby);
                         });
    requestUpdate();
    return;
  }
  if (wallpaperTarget == WallpaperTarget::PowerOff) {
    // Power-off is a complete still image, never the retained-page sticker mode.
    if (doSetSleepCover(wallpaperSource(), false) && wallpaperPicker) finish();
    return;
  }
  if (stickerPicker) {
    if (doSetSticker()) finish();
    return;
  }
  if (!isPng()) {
    // Picking a wallpaper ends here: return to the page that asked, which shows the new picture.
    if (doSetSleepCover(wallpaperSource(), false) && wallpaperPicker) finish();
    return;
  }

  static constexpr StrId options[] = {StrId::STR_NORMAL, StrId::STR_TRANSPARENT};
  static constexpr int optionCount = sizeof(options) / sizeof(options[0]);
  sleepCoverPopup.show(sleepCoverLabel(), options, optionCount, 0, [this](const int index) {
#if defined(RICKYOS_PRODUCT)
    // The regular image viewer and the dedicated picker must enable the same face.
    if ((index == 1 ? doSetSticker() : doSetSleepCover(previewPath.c_str(), false)) && wallpaperPicker) finish();
#else
    if (doSetSleepCover(index == 1 ? filePath.c_str() : previewPath.c_str(), index == 1) && wallpaperPicker) finish();
#endif
  });
  requestUpdate();
}

bool ImageViewerActivity::doSetSticker() {
  if (!imageReady || !isPng()) return false;
  constexpr const char* part = KeepPageFace::kStickerPartPath;
  constexpr const char* backup = KeepPageFace::kStickerBackupPath;
  constexpr const char* destination = KeepPageFace::kStickerPath;
  if (Storage.exists(backup)) {
    // Interrupted previous rename: restore the last complete image first.
    if (!Storage.exists(destination)) {
      if (!Storage.rename(backup, destination)) return false;
    } else if (!Storage.remove(backup))
      return false;
  }
  bool success = !Storage.exists(part) || Storage.remove(part);
  if (success) {
    HalFile input, output;
    success = Storage.openFileForRead("STICKER", filePath, input) && Storage.openFileForWrite("STICKER", part, output);
    if (success) {
      const uint64_t expected = input.fileSize64();
      uint64_t copied = 0;
      uint8_t buffer[1024];  // Bounded cold-path copy scratch, no resident allocation.
      int count;
      while ((count = input.read(buffer, sizeof(buffer))) > 0) {
        if (output.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) {
          success = false;
          break;
        }
        copied += count;
      }
      output.flush();
      success = success && count == 0 && expected > 0 && copied == expected && output.fileSize64() == expected;
    }
  }
  const bool hadPrevious = Storage.exists(destination);
  if (success) {
    success = (!hadPrevious || Storage.rename(destination, backup)) && Storage.rename(part, destination);
  }
  if (success) {
    const uint8_t previous = SETTINGS.rickyStandbyFace;
    SETTINGS.rickyStandbyFace = CrossPointSettings::RICKY_STANDBY_TRANSPARENT;
    success = SETTINGS.saveToFile();
    if (!success) {
      SETTINGS.rickyStandbyFace = previous;
      (void)SETTINGS.saveToFile();
      Storage.remove(destination);
    }
  }
  if (!success && hadPrevious && !Storage.exists(destination) && Storage.exists(backup)) {
    (void)Storage.rename(backup, destination);
  }
  if (success && Storage.exists(backup)) Storage.remove(backup);
  if (Storage.exists(part)) Storage.remove(part);
  GUI.drawPopup(renderer, success ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(1000);
  return success;
}

void ImageViewerActivity::render(RenderLock&&) { sleepCoverPopup.processRender(renderer, mappedInput); }

void ImageViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (sleepCoverPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    if (!sleepCoverPopup.isActive() && !activityManager.isSwitchPending()) onEnter();
    return;
  }

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    if (gallery) {
      filePath = siblingImages[currentImageIndex];
    } else {
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
    }
    onEnter();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (wallpaperPicker) {
      // Nothing was set: the picker reopens its list, where the reader came from.
      ActivityResult cancelled;
      cancelled.isCancelled = true;
      cancelled.data = FilePathResult{filePath};
      setResult(std::move(cancelled));
      finish();
    } else
      activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (tapAction()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showSleepCoverOptions();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
