#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
 public:
  enum class Output { Mono1, Gray2, Gray8 };

 private:
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          Output output, bool crop = true, bool originalThresholds = false);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true, Output output = Output::Gray2,
                                  bool originalThresholds = false);
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop, bool originalThresholds);
  // A book cover at screen size: 8-bit gray on 16-level panels (every gray kept, no
  // dither), the 2-bit dithered picture elsewhere.
  static bool jpegFileToCoverBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop, bool originalThresholds);
  static constexpr bool grayCovers() {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
    return true;
#else
    return false;
#endif
  }
  // Convert with custom target size (for thumbnails and image previews).
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          bool crop = true);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
  // Cover thumbnails: 8-bit gray where the panel shows 16 levels (a 1-bit thumbnail turns
  // dark covers into black blocks), 1-bit elsewhere.
  static bool jpegFileToThumbBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                               int targetMaxHeight);
};
