#pragma once
#include <HalAudioOutput.h>
namespace plus_audio {
bool play(HalAudioOutput::ReadCallback read, uint32_t rate, uint8_t channels, uint8_t volume);
void setVolume(uint8_t volume);
void shutdown();
bool preview(uint8_t volume);
using CaptureCallback = bool (*)(const uint8_t* pcm, size_t bytes);
bool capture(CaptureCallback sink);
bool busy();
bool failed();
}  // namespace plus_audio
