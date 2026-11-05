#include "HalAudioOutput.h"

#include <BoardConfig.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <PlusAudio.h>
namespace HalAudioOutput {
bool present() { return true; }
bool preview(uint8_t volume) { return plus_audio::preview(volume); }
bool playPcm(ReadCallback read, SeekCallback, uint32_t rate, uint8_t channels, uint8_t volume) {
  return plus_audio::play(read, rate, channels, volume);
}
bool restart(uint8_t) { return false; }
void setVolume(uint8_t volume) { plus_audio::setVolume(volume); }
void shutdown() { plus_audio::shutdown(); }
}
#else
namespace HalAudioOutput {
bool present() { return false; }
bool preview(uint8_t) { return false; }
}
#if CROSSPOINT_CAP_SOUND_FEEDBACK

#include <AudioManager.h>
#include <BoardConfig.h>
#include <Logging.h>


namespace HalAudioOutput {
namespace {

constexpr unsigned long AUDIO_RAIL_SETTLE_MS = 10;

AudioManager audio;
bool initialized = false;

bool setBoardPower(const bool enabled) {
  (void)enabled;
  return true;
}

bool begin() {
  if (initialized) return true;
  if (!setBoardPower(true)) {
    LOG_ERR("SND", "Failed to enable board audio power");
    return false;
  }
  if (!audio.begin()) {
    LOG_ERR("SND", "Failed to initialize audio output");
    setBoardPower(false);
    return false;
  }
  initialized = true;
  return true;
}

}  // namespace

bool playPcm(const ReadCallback read, const SeekCallback seek, const uint32_t sampleRate, const uint8_t channels,
             const uint8_t volume) {
  if (!read || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::PcmSource source = {read, seek};
  if (audio.playPcm(source, sampleRate, channels)) return true;
  LOG_ERR("SND", "Failed to play feedback PCM");
  shutdown();
  return false;
}

bool restart(const uint8_t volume) {
  if (!initialized) return false;
  audio.setVolume(volume);
  return audio.requestRestart();
}

void setVolume(const uint8_t volume) {
  if (initialized) audio.setVolume(volume);
}

void shutdown() {
  if (!initialized) return;
  audio.powerDown();
  if (!setBoardPower(false)) LOG_ERR("SND", "Failed to disable board audio power");
  initialized = false;
}

}  // namespace HalAudioOutput

#endif

#endif
