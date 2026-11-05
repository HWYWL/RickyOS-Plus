#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ImageViewerActivity final : public Activity {
 public:
  enum class WallpaperTarget : uint8_t { Standby, PowerOff, Reading, Sticker };
  ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                      bool wallpaperPicker = false, bool stickerPicker = false,
                      WallpaperTarget wallpaperTarget = WallpaperTarget::Standby);
  // From the gallery: flip through `pictures` (full paths) starting at `index`, and set
  // the one shown as the standby, the power-off or the reading background picture, or
  // (target Sticker) as the transparent wallpaper.
  ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<std::string> pictures,
                      int index, WallpaperTarget wallpaperTarget);

  // What the file holds, read from its first bytes: saved pictures often carry the wrong
  // extension (a JPEG named .png), and a decoder chosen by name then rejects them.
  enum class ImageKind : uint8_t { Bmp, Png, Jpeg };

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void loadSiblingImages();
  bool isPng() const;
  bool isJpeg() const;
  bool preparePreview();
  bool doSetSleepCover(const char* sourcePath, bool transparent);
  // Record the shown picture as the source of the screen just set (the gallery marks it).
  void rememberSource() const;
  // The converted picture to copy for this file (the BMP itself when it already is one).
  const char* wallpaperSource() const;
  bool setFromGallery(WallpaperTarget target);
  bool setStickerFromGallery();
  // The tappable action row: one "set" button, or the gallery's power-off, standby and
  // reading-background trio.
  void drawActions() const;
  bool tapAction();
  bool doSetSticker();
  void showSleepCoverOptions();

  std::string filePath;
  std::string previewPath;  // the converted BMP shown for a PNG/JPEG
  ImageKind imageKind = ImageKind::Bmp;
  bool wallpaperPicker = false;
  bool stickerPicker = false;
  WallpaperTarget wallpaperTarget = WallpaperTarget::Standby;
  bool imageReady = false;
  std::vector<std::string> siblingImages;  // names in the file's folder, or full paths in the gallery
  bool gallery = false;
  int currentImageIndex = -1;
  OptionPopup sleepCoverPopup;
};
