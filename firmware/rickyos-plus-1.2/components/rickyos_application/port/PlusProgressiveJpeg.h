#pragma once

#include <HalMemory.h>

#include <string>

class GfxRenderer;
struct RenderConfig;

// Full coefficients, native gray target only. Never falls back to DC-only previews.
bool decodePlusProgressiveJpeg(const std::string& path, GfxRenderer& renderer, const RenderConfig& config);

// One display image (the standby card's cover): any JPEG on the card decoded with full
// coefficients to 8-bit gray, then filled into (center-cropped) or fitted inside a
// boxWidth x boxHeight box. Downscaling averages the source area, so nothing aliases.
// Empty when it does not fit PSRAM; the caller keeps its own image then.
HalMemory::PsramBuffer decodePlusJpegGrayForBox(const std::string& path, int boxWidth, int boxHeight, bool fill,
                                                int& outWidth, int& outHeight);
