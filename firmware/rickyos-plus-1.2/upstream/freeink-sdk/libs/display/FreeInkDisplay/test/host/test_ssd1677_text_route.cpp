#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <vector>
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/PaperMonoDriver.h"
#undef private
#include "src/lut/Ssd1677CombinedAa.h"

using namespace freeink;
using Mode = FreeInkDisplay;
using Bytes = std::vector<uint8_t>;

static Bytes last(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command) return i->bytes;
  return {};
}
static int pixels(const EpdBus& bus) {
  int n = 0;
  for (const auto& w : bus.writes)
    if (w.command == 0x22 && (w.bytes.at(0) & 4)) ++n;
  return n;
}

#if FREEINK_DEVICE_PAPERMONO
int main(int argc, char** argv) {
  assert(argc == 2);
  BoardConfig::ACTIVE.board = BoardConfig::Board::PaperMono;
  BoardConfig::ACTIVE.orientation.mirrorX = true;
  BoardConfig::ACTIVE.orientation.mirrorY = true;
  allocationFailAt = std::atoi(argv[1]);
  FreeInkDisplay d(0, 0, 0, 0, 0, 0);
  d.begin();
  assert(d.supportsTextOnlyCombinedBase() == (allocationFailAt == 0));
  if (allocationFailAt) {
    assert(liveAllocations == 0);
    d.displayBuffer(Mode::FAST_REFRESH, false);
    assert(pixels(d._bus) > 0);
  } else {
    assert(liveAllocations == 8);
    auto& bus = d._bus;
    d.getFrameBuffer()[0] = 0x7f;
    d.displayBuffer(Mode::FAST_REFRESH, false);
    bus.sleepBusyUntilReset = true;
    d.controllerIdle();
    assert(bus.busy);
    const auto resetBeforeWake = bus.resets;
    d.getFrameBuffer()[1] = 0x7f;
    d.displayBuffer(Mode::FAST_REFRESH, false);
    assert(bus.resets > resetBeforeWake && d.displayCommitted());
  }
}
#elif !FREEINK_SSD1677_TEXT_ROUTING
int main(int argc, char** argv) {
  assert(argc == 2);
#if FREEINK_DEVICE_X4CLASSIC
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4Classic;
#endif
  const bool uc = std::strncmp(argv[1], "uc", 2) == 0;
  if (uc)
    BoardConfig::ACTIVE.displayController = std::strcmp(argv[1], "uc8179") == 0
                                                ? BoardConfig::DisplayController::UC8179
                                                : BoardConfig::DisplayController::UC8279;
  FreeInkDisplay d(0, 0, 0, 0, 0, 0);
  d.begin();
  assert(!d.supportsTextOnlyCombinedBase() && !d.supportsReaderTransitions());
  assert(paperMonoDriver()._pendingBw == nullptr);
  if (uc) return 0;  // UC8279 owns its existing grayscale buffers.
  assert(allocationCalls == 0 && liveAllocations == 0);
  std::array<uint8_t, 48000> plane{};
  auto& bus = d._bus;
  bus.clear();
  d.displayGrayscaleBase(Mode::HALF_REFRESH, false, RefreshContext::TextOnlyAntiAliasing);
  assert(pixels(bus) == 1 && last(bus, 0x22) == Bytes{0xD7});
  d.copyGrayscaleBuffers(plane.data(), plane.data());
  d.displayGrayBuffer(false);
  assert(pixels(bus) == 2 && last(bus, 0x22) == Bytes{0xCC});
  const auto textLut = last(bus, 0x32);
  d.cleanupGrayscaleBuffers(plane.data());
  bus.clear();
  d.displayGrayscaleBase(Mode::HALF_REFRESH, false, RefreshContext::ImageReading);
  d.copyGrayscaleBuffers(plane.data(), plane.data());
  d.displayGrayBuffer(false);
  assert(pixels(bus) == 2 && last(bus, 0x32) == textLut);
  assert(allocationCalls == 0);
}
#else
int main(int argc, char** argv) {
  assert(argc == 2);
#if FREEINK_DEVICE_STICKY
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
#elif FREEINK_DEVICE_MURPHY_M4
  BoardConfig::ACTIVE.board = BoardConfig::Board::MurphyM4;
#elif FREEINK_DEVICE_WAVESHARE_EPAPER_397
  BoardConfig::ACTIVE.board = BoardConfig::Board::WaveshareEpaper397;
#elif FREEINK_DEVICE_METALIO_EINK4
  BoardConfig::ACTIVE.board = BoardConfig::Board::MetalioEink4;
#elif FREEINK_DEVICE_X4CLASSIC
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4Classic;
#endif
  const bool uc = std::strncmp(argv[1], "uc", 2) == 0;
  if (uc)
    BoardConfig::ACTIVE.displayController = std::strcmp(argv[1], "uc8179") == 0
                                                ? BoardConfig::DisplayController::UC8179
                                                : BoardConfig::DisplayController::UC8279;
  BoardConfig::ACTIVE.orientation.mirrorX = std::strcmp(argv[1], "rotated") == 0;
  BoardConfig::ACTIVE.orientation.mirrorY = BoardConfig::ACTIVE.orientation.mirrorX;
  allocationFailAt = std::atoi(argv[1]);
  FreeInkDisplay d(0, 0, 0, 0, 0, 0);
#if FREEINK_DEVICE_MURPHY_M4
  if (std::strcmp(argv[1], "batch1") == 0) d.setMurphyM4Batch(MurphyM4Batch::First);
#endif
  d.begin();
  if (uc || allocationFailAt) {
    assert(!d.supportsTextOnlyCombinedBase());
    assert(!d.supportsReaderTransitions() && !d.canUseTextTransition());
    assert(paperMonoDriver()._pendingBw == nullptr);
    if (!uc) assert(liveAllocations == 0);
    assert(d._driver == d._originalDriver);
    return 0;
  }
  assert(d.supportsTextOnlyCombinedBase());
  assert(liveAllocations == 8);
  auto& bus = d._bus;
  auto& combined = paperMonoDriver();
  std::array<uint8_t, 48000> bw, lsb{}, msb{};
  bw.fill(0xff);
  bw[0] = 0x7f;
  lsb[1] = 0x80;
  msb[1] = 0x40;
  const auto stage = [&](Mode::RefreshMode mode) {
    std::memcpy(d.getFrameBuffer(), bw.data(), bw.size());
    d.displayGrayscaleBase(mode, false, RefreshContext::TextOnlyAntiAliasing);
    d.copyGrayscaleBuffers(lsb.data(), msb.data());
  };
  stage(Mode::HALF_REFRESH);
  assert(pixels(bus) == 0);
  d.displayGrayBuffer(false);
  assert(d.displayCommitted());
  const auto scan = last(bus, 0x01);
  d.cleanupGrayscaleBuffers(bw.data());
  bus.clear();
#if FREEINK_SSD1677_TEXT_TURN_AA
  {
    const auto allocated = allocationCalls;
    const auto resets = bus.resets;
    bw[100] = 0;
    lsb[101] = 0x80;
    stage(Mode::FAST_REFRESH);
    d.displayGrayBuffer(false);
    assert(pixels(bus) == 1 && d.displayCommitted() && bus.resets == resets);
    const auto edgeLut = last(bus, 0x32);
    assert(edgeLut.size() == 105 && edgeLut[20] == 0x80 && edgeLut[22] == 0xC0);
    assert(edgeLut[50] == 24 && edgeLut[55] == 8 && edgeLut[60] == 24);
    assert(last(bus, 0x24).at(100) == 0xFF && last(bus, 0x24).at(101) == 0x7F);
    assert(last(bus, 0x26).at(100) == 0xFF && last(bus, 0x26).at(101) == 0x80);
    assert(last(bus, 0x24).at(1) == 0x3F && last(bus, 0x26).at(1) == 0);
    assert(last(bus, 0x11) == Bytes{static_cast<uint8_t>(BoardConfig::ACTIVE.orientation.mirrorX ? 0 : 1)});
    d.cleanupGrayscaleBuffers(bw.data());
    bus.clear();
    stage(Mode::FAST_REFRESH);
    d.displayGrayBuffer(false);
    assert(pixels(bus) == 0);
    d.cleanupGrayscaleBuffers(bw.data());

    // An image switches to the original driver, and the next text entry uses
    // the safe handoff rather than diffing against stale per-pixel history.
    bus.clear();
    std::memcpy(d.getFrameBuffer(), bw.data(), bw.size());
    d.displayGrayscaleBase(Mode::HALF_REFRESH, false, RefreshContext::ImageReading);
    d.copyGrayscaleBuffers(lsb.data(), msb.data());
    d.displayGrayBuffer(false);
    assert(d._driver == d._originalDriver && last(bus, 0x32) != edgeLut);
    d.cleanupGrayscaleBuffers(bw.data());
    bus.clear();
    stage(Mode::FAST_REFRESH);
    d.displayGrayBuffer(false);
    assert(last(bus, 0x32) != edgeLut && d.displayCommitted());
    d.cleanupGrayscaleBuffers(bw.data());
    bus.clear();
    d.setInverted(true);
    stage(Mode::FAST_REFRESH);
    d.displayGrayBuffer(false);
    assert(d._driver == d._originalDriver && last(bus, 0x32) != edgeLut);
    assert(allocationCalls == allocated);
    return 0;
  }
#endif
  bw[100] = 0;
  stage(Mode::FAST_REFRESH);
  assert(pixels(bus) == 0);
  d.displayGrayBuffer(false);
  assert(pixels(bus) == 1);
  const auto initialLut = last(bus, 0x32);
  assert(initialLut.size() == 105);
  assert(last(bus, 0x11) == Bytes{static_cast<uint8_t>(BoardConfig::ACTIVE.orientation.mirrorX ? 0 : 1)});
  assert(scan.at(2) == (BoardConfig::ACTIVE.orientation.mirrorY ? 3 : 2));
  assert(last(bus, 0x4F) == (Bytes{0xdf, 1}));
  assert(last(bus, 0x24).at(100) == 0xff);  // native byte order, no Paper Mono mount transform

  d.cleanupGrayscaleBuffers(bw.data());
  assert(!d._textAaPending);
  // Gray failure commits B/W; cancellation does not commit any pixels.
  bus.clear();
  bw[101] = 0;
  std::memcpy(d.getFrameBuffer(), bw.data(), bw.size());
  d.displayGrayscaleBase(Mode::FAST_REFRESH, false, RefreshContext::TextOnlyAntiAliasing);
  d.copyGrayscaleLsbBuffers(lsb.data());
  d.displayGrayBuffer(false);
  assert(!last(bus, 0x24).empty() && last(bus, 0x32).empty());
  d.cleanupGrayscaleBuffers(bw.data());
  bus.clear();
  stage(Mode::FAST_REFRESH);
  d.abortPostRefresh();
  d.cleanupGrayscaleBuffers(bw.data());
  assert(pixels(bus) == 0);

  // Compare the complete image transaction to the ordinary driver, including
  // both intermediate tones, LUT, power and cleanup commands.
  auto* original = d._originalDriver;
  EpdBus expected;
  original->begin(expected);
  expected.clear();
  original->displayGrayscaleBaseWithContext(expected, bw.data(), RefreshMode::Fast, false, RefreshContext::Normal);
  original->copyGrayscaleLsb(expected, lsb.data());
  original->copyGrayscaleMsb(expected, msb.data());
  original->displayGray(expected, bw.data(), false, nullptr, false);
  original->cleanupGrayscaleBuffers(expected, bw.data());
  d.selectTextAaDriver(false);
  bus.clear();
  std::memcpy(d.getFrameBuffer(), bw.data(), bw.size());
  d.displayGrayscaleBase(Mode::FAST_REFRESH, false);
  d.copyGrayscaleBuffers(lsb.data(), msb.data());
  assert(last(bus, 0x24) == Bytes(lsb.begin(), lsb.end()));
  assert(last(bus, 0x26) == Bytes(msb.begin(), msb.end()));
  d.displayGrayBuffer(false);
  d.cleanupGrayscaleBuffers(bw.data());
  assert(bus.writes.size() == expected.writes.size());
  for (size_t i = 0; i < bus.writes.size(); ++i) {
    assert(bus.writes[i].command == expected.writes[i].command);
    assert(bus.writes[i].bytes == expected.writes[i].bytes);
  }
  assert(d._driver == original);

  // A normal pending async waveform must finish before the next page checks BUSY.
  if (original->supportsAsyncDisplay()) {
    bus.clear();
    d.displayBufferAsync(Mode::FAST_REFRESH);
#if FREEINK_DEVICE_STICKY
    assert(d._refreshPending);
#endif
    if (d._refreshPending) {
      assert(bus.busy);
      const auto firstActivation = bus.activation;
      d.displayBufferAsync(Mode::FAST_REFRESH);
      assert(bus.activation > firstActivation && d._refreshPending);
      d.finishDisplayAsync();
      assert(!bus.busy && !d._refreshPending);
    }
  }
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC
  // A timed-out async refresh cannot run the deferred power-off SPI sequence.
  bus.clear();
  d.triggerDisplayAsync(Mode::FAST_REFRESH, true);
  assert(d._refreshPending);
  bus.failAt = bus.activation;
  const auto timedOutWrites = bus.writes.size();
  d.finishDisplayAsync();
  assert(bus.busy && bus.writes.size() == timedOutWrites);
  bus.failAt = 0;
  bus.busy = false;
#endif

  // Deep sleep may hold BUSY until hardware reset; both routing directions must recover.
  bus.sleepBusyUntilReset = true;
  const auto firstReset = bus.resets;
  stage(Mode::FAST_REFRESH);
  assert(d._driver == &combined && d.combinesGrayscaleBase() && bus.resets > firstReset);
  d.displayGrayBuffer(false);
  d.cleanupGrayscaleBuffers(bw.data());
  const auto secondReset = bus.resets;
  d.selectTextAaDriver(false);
  assert(d._driver == original && !bus.busy && bus.resets > secondReset);
  bus.sleepBusyUntilReset = false;

  // Idle sleep retains the selected text driver but invalidates its controller registers.
  stage(Mode::FAST_REFRESH);
  d.displayGrayBuffer(false);
  d.cleanupGrayscaleBuffers(bw.data());
  bus.sleepBusyUntilReset = true;
  assert(!combined._needsFull);
  d.controllerIdle();
  assert(bus.busy && !d._textDriverReady);
  const auto idleReset = bus.resets;
  stage(Mode::FAST_REFRESH);
  assert(bus.resets > idleReset && d.combinesGrayscaleBase());
  assert(!combined._needsFull);
  d.displayGrayBuffer(false);
  d.cleanupGrayscaleBuffers(bw.data());
  bus.sleepBusyUntilReset = false;
  d.selectTextAaDriver(false);

  // If reset cannot release BUSY, do not send controller commands or claim a text transaction.
  bus.sleepBusyUntilReset = true;
  bus.resetKeepsBusy = true;
  d.displayGrayscaleBase(Mode::FAST_REFRESH, false, RefreshContext::TextOnlyAntiAliasing);
  assert(bus.busy && !d._textDriverReady && !d.combinesGrayscaleBase());
  assert(bus.writes.back().command == 0x10);
  bus.resetKeepsBusy = false;
  stage(Mode::FAST_REFRESH);
  assert(d._driver == &combined && d.combinesGrayscaleBase());
  d.displayGrayBuffer(false);
  d.cleanupGrayscaleBuffers(bw.data());
  d.selectTextAaDriver(false);
  bus.sleepBusyUntilReset = false;

  // An actual BUSY timeout must not make the original grayscale cleanup write RAM.
  bus.busy = true;
  const auto stoppedOriginal = bus.writes.size();
  d.cleanupGrayscaleBuffers(bw.data());
  assert(bus.writes.size() == stoppedOriginal && !d._textDriverReady);
  bus.busy = false;

  stage(Mode::FAST_REFRESH);
  d.displayGrayBuffer(true);
  d.cleanupGrayscaleBuffers(bw.data());
  assert(d._driver == &combined);
#if FREEINK_DEVICE_METALIO_EINK4
  assert(last(bus, 0x22) == Bytes{0x83});
#endif
  d.deepSleep();
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  assert(last(bus, 0x10) == Bytes{0x01});
#else
  assert(last(bus, 0x10) == Bytes{0x03});
#endif
  bus.clear();
  stage(Mode::FAST_REFRESH);
  d.displayGrayBuffer(false);
  assert(pixels(bus) >= 2);
  d.cleanupGrayscaleBuffers(bw.data());
  // Timeouts in both the custom waveform and original corrective B/W must
  // stop all subsequent SPI writes and require a clean recovery.
  for (auto mode : {Mode::FAST_REFRESH, Mode::HALF_REFRESH}) {
    bus.clear();
    bw[102] ^= 0xff;
    stage(mode);
    bus.failAt = bus.activation + 1;
    d.displayGrayBuffer(true);
    assert(bus.busy && !d.displayCommitted());
    const auto stopped = bus.writes.size();
    d.cleanupGrayscaleBuffers(bw.data());
    d.controllerIdle();
    d.deepSleep();
    assert(bus.writes.size() == stopped);
    bus.failAt = 0;
    bus.busy = false;
    bus.clear();
    stage(Mode::FAST_REFRESH);
    d.displayGrayBuffer(false);
    assert(d.displayCommitted() && pixels(bus) >= 2);
    d.cleanupGrayscaleBuffers(bw.data());
  }
#if FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_STICKY || FREEINK_DEVICE_MURPHY_M4 || \
    FREEINK_DEVICE_WAVESHARE_EPAPER_397
#if FREEINK_SSD1677_READER_TRANSITIONS
  constexpr bool experiment = true;
#else
  constexpr bool experiment = false;
#endif
#if FREEINK_DEVICE_METALIO_EINK4
  const int correctivePixels = 3;
#else
  const int correctivePixels = 2;
#endif
  assert(d.supportsReaderTransitions() == experiment);
  assert(d.supportsContinuousImageReading() ==
         (experiment && BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEink4));
  const auto transition = [&](Mode::RefreshMode mode = Mode::FAST_REFRESH) {
    std::memcpy(d.getFrameBuffer(), bw.data(), bw.size());
    d.displayGrayscaleBase(mode, false, RefreshContext::TextOnlyAntiAliasing);
    d.copyGrayscaleBuffers(lsb.data(), msb.data());
  };
  for (const bool image : {false, true}) {
    d.displayBuffer(image ? Mode::HALF_REFRESH : Mode::FULL_REFRESH, false,
                    image ? RefreshContext::ImageReading : RefreshContext::Normal);
    if (image) {
      d.copyGrayscaleBuffers(lsb.data(), msb.data());
      d.displayGrayBuffer(false);
      d.cleanupGrayscaleBuffers(bw.data());
    }
    assert(d.canUseTextTransition() == experiment);
    bus.clear();
    transition();
    assert(pixels(bus) == 0);  // All planes are staged before the first pixel activation.
    d.displayGrayBuffer(false);
    assert(pixels(bus) == (experiment ? 1 : correctivePixels));
    assert(last(bus, 0x32) == initialLut);  // Existing waveform, no changed-only tuning.
    assert(d.displayCommitted() && !combined._needsFull);
    d.cleanupGrayscaleBuffers(bw.data());
    assert(!d.canUseTextTransition());
    bus.clear();
    bw[100] ^= 0x80;
    stage(Mode::FAST_REFRESH);
    d.displayGrayBuffer(false);
    assert(pixels(bus) == 1);
    d.cleanupGrayscaleBuffers(bw.data());
    bus.clear();
    stage(Mode::HALF_REFRESH);  // Deferred periodic/manual clean still runs in full.
    d.displayGrayBuffer(false);
    assert(pixels(bus) == correctivePixels);
    d.cleanupGrayscaleBuffers(bw.data());
  }
  // RAM synchronization preserves optical gray; resync and original-driver
  // failures revoke it, even after the external BUSY signal is released.
  d.displayBuffer(Mode::FULL_REFRESH);
  d.copyGrayscaleBuffers(lsb.data(), msb.data());
  bus.failAt = bus.activation + 1;
  d.displayGrayBuffer(false);
  assert(!d.canUseTextTransition());
  assert(d._driver->opticalState() == OpticalState::Unknown);
  const auto originalFailedWrites = bus.writes.size();
  d.cleanupGrayscaleBuffers(bw.data());
  assert(bus.writes.size() == originalFailedWrites);
  bus.failAt = 0;
  bus.busy = false;
  assert(!d.canUseTextTransition());
  d.displayBuffer(Mode::FULL_REFRESH);
  assert(d.canUseTextTransition() == experiment);
  d.requestResync();
  assert(!d.canUseTextTransition());
  d.displayBuffer(Mode::FULL_REFRESH);
  d.displayBufferAsync(Mode::FAST_REFRESH);
  if (d._refreshPending) {
    bus.failAt = bus.activation;
    d.finishDisplayAsync();
    assert(original->opticalState() == OpticalState::Unknown);
    bus.failAt = 0;
    bus.busy = false;
    original->displayFinish(bus, bw.data());
    assert(original->opticalState() == OpticalState::Unknown);  // Late completion cannot revive a failed frame.
  }
  d.displayBuffer(Mode::FULL_REFRESH);
  d.displayBufferAsync(Mode::FAST_REFRESH);
  if (d._refreshPending) {
    assert(!d.canUseTextTransition());
    d.finishDisplayAsync();
    assert(d.canUseTextTransition() == experiment);
  }
  // Explicit HALF/FULL cannot be weakened, even with an incorrectly supplied transition context.
  for (const auto mode : {Mode::HALF_REFRESH, Mode::FULL_REFRESH}) {
    d.displayBuffer(Mode::FULL_REFRESH);
    bus.clear();
    transition(mode);
    d.displayGrayBuffer(false);
    assert(pixels(bus) == (mode == Mode::FULL_REFRESH ? 2 : correctivePixels));
    d.cleanupGrayscaleBuffers(bw.data());
  }
  d.displayBuffer(Mode::FULL_REFRESH);
#if FREEINK_DEVICE_STICKY || FREEINK_DEVICE_WAVESHARE_EPAPER_397
  // These vendor BW sequences power off; their gray sequences keep rails on.
  d.copyGrayscaleBuffers(lsb.data(), msb.data());
  d.displayGrayBuffer(false);
  d.cleanupGrayscaleBuffers(bw.data());
#else
  d.displayBuffer(Mode::FAST_REFRESH, false);
#endif
  bus.clear();
  bus.resetClearsFailure = true;
  bus.failAt = bus.activation + 1;  // Fail outgoing original-driver power-off, before text begin().
  transition();
  d.displayGrayBuffer(false);
  assert(pixels(bus) == correctivePixels);  // Successful reset must not resurrect a failed handoff's permission.
  d.cleanupGrayscaleBuffers(bw.data());
  bus.resetClearsFailure = false;
  bus.failAt = 0;

  d.displayBuffer(Mode::FULL_REFRESH);
  bus.clear();
  transition();
  d.abortPostRefresh();
  d.beginDisplayWork();  // Same cancellation sequence as HalDisplay::cancelGrayscale().
  d.cleanupGrayscaleBuffers(bw.data());
  assert(pixels(bus) == 0);
  assert(combined._transitionSource == OpticalState::Unknown);
  transition();  // A canceled handoff must not leak its permission to a later frame.
  d.displayGrayBuffer(false);
  assert(pixels(bus) == correctivePixels);
  d.cleanupGrayscaleBuffers(bw.data());

  d.displayBuffer(Mode::FULL_REFRESH);
  bus.clear();
  transition();
  bus.failAt = bus.activation + 1;
  d.displayGrayBuffer(false);
  assert(!d.displayCommitted());
  const auto failedWrites = bus.writes.size();
  d.cleanupGrayscaleBuffers(bw.data());
  assert(bus.writes.size() == failedWrites);
  bus.failAt = 0;
  bus.busy = false;
  bus.clear();
  transition();
  d.displayGrayBuffer(false);
  assert(pixels(bus) == correctivePixels);  // Fault recovery uses the original endpoint clean.
  d.cleanupGrayscaleBuffers(bw.data());

  d.deepSleep();
  assert(!d.canUseTextTransition());
  bus.clear();
  transition();
  d.displayGrayBuffer(false);
  assert(pixels(bus) == correctivePixels);  // Unknown glass after power/sleep cannot take the shortcut.
  d.cleanupGrayscaleBuffers(bw.data());
  d.setInverted(true);
  assert(!d.supportsReaderTransitions() && !d.canUseTextTransition());
#endif
  assert(allocationCalls == 8);
}

#endif
