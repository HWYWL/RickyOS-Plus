#include "HalRecorder.h"
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalAudioOutput.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PlusAudio.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "RecorderWav.h"

namespace HalRecorder {
namespace {
State current = State::Idle;
HalFile file;
char path[80]{}, temporary[88]{};
std::atomic<uint32_t> bytes{0};
std::atomic<bool> ioFailed{false};
uint32_t playbackBytes = 0;
bool isName(const char* name) {
  const size_t n = strlen(name);
  return n > 8 && n < 40 && strncmp(name, "REC_", 4) == 0 && strcmp(name + n - 4, ".wav") == 0 && !strchr(name, '/') &&
         !strchr(name, '\\');
}
bool sink(const uint8_t* data, size_t length) {
  const uint32_t before = bytes.load();
  if (length > recorder_wav::maxBytes - before) return false;
  const size_t written = file.write(data, length);
  bytes.store(before + written);
  if (written != length) {
    ioFailed.store(true);
    return false;
  }
  return true;
}
int source(uint8_t* dst, size_t length) {
  const uint32_t pos = bytes.load();
  if (pos >= playbackBytes) return 0;
  const size_t wanted = std::min<size_t>(length, playbackBytes - pos);
  const int n = file.read(dst, wanted);
  if (n <= 0 || static_cast<size_t>(n) > wanted) {
    ioFailed.store(true);
    return -1;
  }
  bytes.store(pos + n);
  return n;
}
void finish() {
  const bool recording = current == State::Recording;
  bool bad = ioFailed.load() || (plus_audio::failed() && bytes.load() < recorder_wav::maxBytes);
  if (recording && !bytes.load() && !bad) {
    // Stopping during the module's setup delay is a normal cancellation.
    const bool closed = file.close();
    const bool removed = Storage.remove(temporary);
    current = closed && removed ? State::Idle : State::Error;
    return;
  }
  if (recording) {
    // Worker is joined before accessing its file. Salvage complete PCM on failure.
    const uint32_t n = bytes.load() & ~uint32_t(1);
    uint8_t header[44];
    recorder_wav::header(header, n);
    const bool finalized =
        n && file.truncate(uint64_t(n) + 44) && file.seekSet(0) && file.write(header, sizeof(header)) == sizeof(header);
    file.flush();
    const bool closed = file.close();
    if (finalized && closed && Storage.rename(temporary, path)) {
      LOG_INF("REC", "Saved %s pcm=%u partial=%d", path, n, bad);
    } else {
      bad = true;
      if (!n) Storage.remove(temporary);
      LOG_ERR("REC", "Finalize failed %s pcm=%u", temporary, n);
    }
  } else {
    file.close();
  }
  current = bad ? State::Error : recording ? State::Saved : State::Idle;
}
}  // namespace
State state() { return current; }
bool active() { return current == State::Recording || current == State::Playing; }
bool preparing() { return current == State::Recording && bytes.load() == 0; }
uint32_t seconds() { return bytes.load() / (recorder_wav::rate * 2); }
void poll() {
  if (!active()) return;
  if (current == State::Recording && bytes.load() >= recorder_wav::maxBytes) {
    plus_audio::shutdown();
    finish();
  } else if (!plus_audio::busy())
    finish();
}
void stop() {
  if (!active()) return;
  plus_audio::shutdown();
  finish();
}
bool startRecording() {
  stop();
  HalAudioOutput::shutdown();
  current = State::Error;
  if (!Storage.ready() || !Storage.ensureDirectoryExists("/Recordings")) return false;
  uint64_t total, free;
  if (!Storage.getSpace(total, free) || free < 1024 * 1024) return false;
  const time_t now = time(nullptr);
  struct tm t{};
  localtime_r(&now, &t);
  char date[24];
  strftime(date, sizeof(date), "%Y%m%d_%H%M%S", &t);
  bool opened = false;
  for (unsigned i = 0; i < 100; ++i) {
    snprintf(path, sizeof(path), "/Recordings/REC_%s_%02u.wav", date, i);
    snprintf(temporary, sizeof(temporary), "%s.part", path);
    if (Storage.exists(path)) continue;
    file = Storage.open(temporary, O_WRONLY | O_CREAT | O_EXCL);
    if (file) {
      opened = true;
      break;
    }
  }
  if (!opened) return false;
  uint8_t h[44];
  recorder_wav::header(h, 0);
  if (file.write(h, sizeof(h)) != sizeof(h)) {
    file.close();
    Storage.remove(temporary);
    return false;
  }
  bytes.store(0);
  ioFailed.store(false);
  if (!plus_audio::capture(sink)) {
    file.close();
    Storage.remove(temporary);
    return false;
  }
  current = State::Recording;
  LOG_INF("REC", "Recording %s 16kHz mono", path);
  return true;
}
bool play(const char* name, uint8_t volume) {
  stop();
  HalAudioOutput::shutdown();
  current = State::Error;
  if (!name || !isName(name)) return false;
  snprintf(path, sizeof(path), "/Recordings/%s", name);
  file = Storage.open(path);
  uint8_t h[44];
  if (!file || file.read(h, sizeof(h)) != sizeof(h) || !recorder_wav::valid(h, file.fileSize64())) {
    file.close();
    return false;
  }
  playbackBytes = recorder_wav::get32(h + 40);
  bytes.store(0);
  ioFailed.store(false);
  if (!plus_audio::play(source, 16000, 1, volume)) {
    file.close();
    return false;
  }
  current = State::Playing;
  return true;
}
size_t list(size_t offset, Entry* entries, size_t capacity, bool& more) {
  more = false;
  if (active() || !entries || !capacity) return 0;
  auto dir = Storage.open("/Recordings");
  if (!dir || !dir.isDirectory()) return 0;
  size_t found = 0, count = 0;
  for (;;) {
    auto entry = dir.openNextFile();
    if (!entry) break;
    char name[40]{};
    entry.getName(name, sizeof(name));
    if (entry.isDirectory() || !isName(name)) continue;
    uint8_t h[44];
    if (entry.read(h, sizeof(h)) != sizeof(h) || !recorder_wav::valid(h, entry.fileSize64())) continue;
    if (found++ < offset) continue;
    if (count == capacity) {
      more = true;
      break;
    }
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
    entries[count].seconds = recorder_wav::get32(h + 40) / (recorder_wav::rate * 2);
    ++count;
  }
  return count;
}
}  // namespace HalRecorder
#endif
