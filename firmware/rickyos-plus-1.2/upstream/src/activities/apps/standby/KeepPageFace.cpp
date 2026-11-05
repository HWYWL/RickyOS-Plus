#include "KeepPageFace.h"

#include <Epub/converters/PngToFramebufferConverter.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

void KeepPageFace::recoverSticker() {
  // FAT rename is not a power-loss transaction. Never display an incomplete .part;
  // recover the previous complete PNG if installation stopped between renames.
  if (!Storage.exists(kStickerPath) && Storage.exists(kStickerBackupPath) &&
      !Storage.rename(kStickerBackupPath, kStickerPath)) {
    LOG_ERR("STANDBY", "Cannot recover previous sticker");
  }
}

void KeepPageFace::onEnter() {
  drawn_ = false;
  if (sticker_) recoverSticker();
}

bool KeepPageFace::drawRetained(GfxRenderer& renderer, const Rect& viewport, const bool sticker,
                                const bool poweredOff) {
  if (viewport.width <= 0 || viewport.height <= 0) return false;
  if (!renderer.beginRetainedGrayscale16()) return false;
  if (sticker && Storage.exists(kStickerPath)) {
    const std::string path{kStickerPath};  // Bounded SSO path; decoder row scratch is released before commit.
    PngToFramebufferConverter decoder;
    ImageDimensions dims{};
    bool decoded = false;
    if (decoder.getDimensions(path, dims) && dims.width > 0 && dims.height > 0) {
      RenderConfig config{};
      config.maxWidth = viewport.width;
      config.maxHeight = viewport.height;
      float ratio = static_cast<float>(dims.width) / dims.height;
      const float screenRatio = static_cast<float>(viewport.width) / viewport.height;
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::CROP) {
        if (ratio > screenRatio)
          config.sourceCropX = 1.0f - screenRatio / ratio;
        else
          config.sourceCropY = 1.0f - ratio / screenRatio;
        ratio = screenRatio;
      }
      const int width = std::min(viewport.width, static_cast<int>(viewport.height * ratio));
      const int height = std::min(viewport.height, static_cast<int>(viewport.width / ratio));
      config.x = viewport.x + (viewport.width - width) / 2;
      config.y = viewport.y + (viewport.height - height) / 2;
      config.maxWidth = std::max(1, width);
      config.maxHeight = std::max(1, height);
      config.useExactDimensions = true;
      config.preserveAlpha = true;
      config.useDithering = false;
      config.output = DecodeOutput::NativeGrayscale16;
      const GfxRenderer::ClipScope clip(renderer, viewport.x, viewport.y, viewport.width, viewport.height);
      decoded = decoder.decodeToFramebuffer(path, renderer, config);
    }
    if (!decoded) {
      // Decode can fail after drawing rows: discard ALL of them, not a partial sticker.
      LOG_ERR("STANDBY", "Sticker decode failed; keeping the original page");
      renderer.cancelGrayscale16();
      if (!renderer.beginRetainedGrayscale16()) return false;
    }
  }
  // Standby is just the retained page and optional PNG; only power-off has a label.
  if (poweredOff) GUI.drawRickyStandbyIndicator(renderer, true);
  const bool committed = renderer.commitGrayscale16();
  renderer.cancelGrayscale16();
  return committed;
}

bool KeepPageFace::renderNative(GfxRenderer& renderer, const Rect& viewport) {
  if (drawn_) return true;  // Never alpha-composite the same sticker twice on timer/input redraws.
  drawn_ = drawRetained(renderer, viewport, sticker_);
  if (!drawn_) {
    // Unsupported/inverted/unknown native baseline: safe existing keep-page behavior.
    // No sticker decode in this path, so no partial overlay or extra screenshot allocation.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    drawn_ = true;
  }
  return true;
}
