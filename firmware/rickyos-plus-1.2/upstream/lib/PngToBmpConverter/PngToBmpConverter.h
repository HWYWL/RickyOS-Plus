#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool preserveTransparency = false,
                                         bool originalThresholds = false, bool gray8 = false);
  static bool pngFileToBmpFileInternal(const char* pngPath, const char* bmpPath, bool crop, bool preserveTransparency,
                                       bool gray8 = false);

 public:
  static bool pngFileToBmpFile(const char* pngPath, const char* bmpPath, bool crop = true);
  // 8-bit gray BMP (no dithering): keeps every gray for panels with native 16-level output.
  static bool pngFileToGray8BmpFile(const char* pngPath, const char* bmpPath, bool crop = true);
  static bool pngFileToTransparentBmpFile(const char* pngPath, const char* bmpPath, bool crop = true);
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  // A book cover at screen size: 8-bit gray on 16-level panels (see JpegToBmpConverter).
  static bool pngFileToCoverBmpStream(HalFile& pngFile, Print& bmpOut, bool crop, bool originalThresholds);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Cover thumbnails: 8-bit gray on 16-level panels, 1-bit elsewhere (see JpegToBmpConverter).
  static bool pngFileToThumbBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
