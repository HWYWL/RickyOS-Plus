"""Checked adapters for timed task notification and retained native-gray frames.

Only simulator_rickyos uses this. The maintained input is this script, not the
generated dependency under .pio/. Remove each adapter when the pinned simulator
provides the matching capability.
"""

from pathlib import Path

OLD_NOTIFY = '''// Block until notified (simulates ulTaskNotifyTake with clear-on-exit).
inline uint32_t ulTaskNotifyTake(int /*clearOnExit*/,
                                 uint32_t /*ticksToWait*/) {
  auto *h = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(h->mtx);
  h->cv.wait(lk, [h] { return h->notifyCount > 0; });
  h->notifyCount--;
  return 1;
}'''

NEW_NOTIFY = '''// RICKYOS_TIMED_TASK_NOTIFY: FreeRTOS-compatible wait and counter semantics.
inline uint32_t ulTaskNotifyTake(int clearOnExit, uint32_t ticksToWait) {
  auto *h = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(h->mtx);
  const auto notified = [h] { return h->notifyCount > 0; };
  if (ticksToWait == portMAX_DELAY) {
    h->cv.wait(lk, notified);
  } else if (!h->cv.wait_for(lk, std::chrono::milliseconds(
                 static_cast<uint64_t>(ticksToWait) * portTICK_PERIOD_MS), notified)) {
    return 0;
  }
  const uint32_t count = h->notifyCount;
  if (clearOnExit) h->notifyCount = 0;
  else --h->notifyCount;
  return count;
}'''


def patch_task_notify(path):
    path = Path(path)
    source = path.read_text()
    if NEW_NOTIFY in source:
        return False
    if source.count(OLD_NOTIFY) != 1:
        raise RuntimeError('Simulator task shim changed; review the timed notification adapter.')
    path.write_text(source.replace(OLD_NOTIFY, NEW_NOTIFY))
    return True


RETAINED_GRAY = '''// RICKYOS_RETAINED_GRAY: reconstruct from the last visible frame, not staging pixels.
uint8_t* HalDisplay::beginRetainedGrayscale16() {
#if FREEINK_DEVICE_READPICO
  if (grayscale16Active || frameBufferLent || isInverted()) return nullptr;
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  grayscale16Active = true;
  for (size_t i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; ++i) {
    const uint8_t gray = static_cast<uint8_t>(pixelBuf[i] & 255);
    const uint8_t level = (gray + 8) / 17;
    const unsigned shift = (i & 1) * 4;
    grayscale16Buffer[i / 2] = (grayscale16Buffer[i / 2] & ~(15u << shift)) | (level << shift);
    const uint8_t bit = 0x80u >> (i & 7);
    if (level >= 8) frameBufferStorage[i / 8] |= bit;
    else frameBufferStorage[i / 8] &= ~bit;
  }
  return grayscale16Buffer.data();
#else
  return nullptr;
#endif
}

'''


def patch_retained_gray(dependency):
    dependency = Path(dependency)
    header = dependency / 'src/HalDisplay.h'
    source = header.read_text()
    declaration = '  uint8_t* beginRetainedGrayscale16();\n'
    anchor = '  uint8_t* beginGrayscale16();\n'
    if declaration not in source:
        if source.count(anchor) != 1:
            raise RuntimeError('Simulator native gray declaration changed')
        header.write_text(source.replace(anchor, anchor + declaration))
    driver = dependency / 'src/HalDisplay.cpp'
    source = driver.read_text()
    anchor = 'uint8_t* HalDisplay::beginGrayscale16() {\n'
    if RETAINED_GRAY not in source:
        if 'RICKYOS_RETAINED_GRAY' in source or source.count(anchor) != 1:
            raise RuntimeError('Simulator retained gray adapter changed')
        driver.write_text(source.replace(anchor, RETAINED_GRAY + anchor))


if 'Import' in globals():
    Import('env')  # noqa: F821 -- supplied by PlatformIO/SCons
    if env.subst('$PIOENV') == 'simulator_rickyos':  # noqa: F821
        dependency = Path(env.subst('$PROJECT_LIBDEPS_DIR')) / 'simulator_rickyos' / 'simulator'  # noqa: F821
        patch_task_notify(dependency / 'src/freertos/task.h')
        patch_retained_gray(dependency)
