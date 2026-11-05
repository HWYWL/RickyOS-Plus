#include "ImageDecoderFactory.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }
#if FREEINK_DEVICE_METALIO_PLUS
  // Legacy section caches may still name PNG bytes .jpg. Prefer a bounded
  // content probe after extraction; keep suffix-based probing for lazy files.
  HalFile file;
  if (Storage.openFileForRead("DEC", imagePath, file)) {
    uint8_t header[8]{};
    const int count = file.read(header, sizeof(header));
    const uint8_t png[8]{137, 80, 78, 71, 13, 10, 26, 10};
    if (count == 8 && std::equal(header, header + 8, png))
      ext = ".png";
    else if (count >= 3 && header[0] == 255 && header[1] == 216 && header[2] == 255)
      ext = ".jpg";
  }
#endif

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder = makeUniqueNoThrow<JpegToFramebufferConverter>();
      if (!jpegDecoder) {
        LOG_ERR("DEC", "OOM creating JPEG decoder");
        return nullptr;
      }
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder = makeUniqueNoThrow<PngToFramebufferConverter>();
      if (!pngDecoder) {
        LOG_ERR("DEC", "OOM creating PNG decoder");
        return nullptr;
      }
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
