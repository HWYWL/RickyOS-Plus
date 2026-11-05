// Desktop format preview only: never open the host microphone/audio device.
#if defined(SIMULATOR) && defined(RICKYOS_KINDLE_IMPORT)
#include <HalAudioOutput.h>
namespace HalAudioOutput {
bool present() { return false; }
bool preview(uint8_t) { return false; }
bool playPcm(ReadCallback, SeekCallback, uint32_t, uint8_t, uint8_t) { return false; }
bool restart(uint8_t) { return false; }
void setVolume(uint8_t) {}
void shutdown() {}
}  // namespace HalAudioOutput
#endif
