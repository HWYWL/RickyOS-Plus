#include <array>

#include "driver/PaperMonoDriver.cpp"

using namespace freeink;
using Mode = RefreshMode;

int pixelRefreshes(const EpdBus& bus) {
  int n = 0;
  for (auto value : bus.sequences())
    if (value & 0x04) ++n;
  return n;
}

int main() {
  // Failure of any one of the eight allocations leaves nothing behind for fallback.
  for (int failure = 1; failure <= 8; ++failure) {
    allocationCalls = 0;
    allocationFailAt = failure;
    PaperMonoDriver failed;
    assert(!failed.prepareBuffers());
    assert(liveAllocations == 0);
  }
  allocationFailAt = 0;
  PaperMonoDriver d;
  assert(d.prepareBuffers());
  const int allocated = allocationCalls;
  assert(d.prepareBuffers() && allocationCalls == allocated);
  BoardConfig::ACTIVE.orientation.mirrorX = !FREEINK_SSD1677_TEXT_ROUTING;
  BoardConfig::ACTIVE.orientation.mirrorY = !FREEINK_SSD1677_TEXT_ROUTING;
  EpdBus bus;
  d.begin(bus);
  std::array<uint8_t, 48000> bw, gray{}, empty{};
  bw.fill(0xFF);
  bw[0] = 0x7F;
  bw.back() = 0xFE;
  d.display(bus, bw.data(), nullptr, Mode::Full, false);
#if FREEINK_SSD1677_TEXT_ROUTING
  assert(bus.sequences() == std::vector<uint8_t>{0xF7});
  assert(bus.last(0x11) == std::vector<uint8_t>{0x01});
  assert(bus.last(0x4E) == (std::vector<uint8_t>{0, 0}));
  assert(bus.last(0x4F) == (std::vector<uint8_t>{0xDF, 1}));
  assert(bus.last(0x24) == std::vector<uint8_t>(bw.begin(), bw.end()));
#else
  assert(bus.last(0x11) == std::vector<uint8_t>{0x02});
#endif
  bus.clear();
  bw[100] = 0x00;
  gray[101] = 0x40;
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  assert(pixelRefreshes(bus) == 0);
  // Incomplete coverage cannot count as a ready plane.
  d.writeGrayscalePlaneStrip(bus, GrayPlane::Lsb, gray.data(), 0, 80);
  d.copyGrayscaleMsb(bus, empty.data());
  assert(pixelRefreshes(bus) == 0);
  d.displayGray(bus, bw.data(), false, nullptr, false);
  assert(bus.last(0x32).empty());  // B/W fallback, never stale/partial gray.
  assert(pixelRefreshes(bus) >= 1);

  bus.clear();
  bw[200] = 0x00;
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  d.copyGrayscaleLsb(bus, gray.data());
  d.copyGrayscaleMsb(bus, empty.data());
  assert(pixelRefreshes(bus) == 0);
  d.displayGray(bus, bw.data(), false, nullptr, false);
  assert(pixelRefreshes(bus) == 1);
  assert(bus.last(0x32).size() == 105);
  const auto selectors = bus.last(0x24);
#if FREEINK_SSD1677_TEXT_ROUTING
  assert(selectors[200] == 0xFF);        // black -> target class 3
  assert((selectors[101] & 0x40) == 0);  // gray -> target class 2
#endif
  const auto beforeCleanup = pixelRefreshes(bus);
  d.cleanupGrayscaleBuffers(bus, bw.data());
  assert(pixelRefreshes(bus) == beforeCleanup);

  // Cancelled and obsolete planes never reach the screen; next frame still works.
  bus.clear();
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  d.copyGrayscaleLsb(bus, gray.data());
  d.copyGrayscaleMsb(bus, empty.data());
  d.abortPostRefresh();
  d.cleanupGrayscaleBuffers(bus, bw.data());
  assert(pixelRefreshes(bus) == 0);
  bw[300] = 0;
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  d.cleanupGrayscaleBuffers(bus, bw.data());
  assert(pixelRefreshes(bus) >= 1 && bus.last(0x32).empty());

#if FREEINK_SSD1677_TEXT_ROUTING
  // HALF/manual cleanup retains a real F7 even with gray submitted afterwards.
  bus.clear();
  d.displayGrayscaleBase(bus, bw.data(), Mode::Half, false);
  assert(pixelRefreshes(bus) == 0);
  d.copyGrayscaleLsb(bus, gray.data());
  d.copyGrayscaleMsb(bus, empty.data());
  d.displayGray(bus, bw.data(), false, nullptr, false);
  assert(pixelRefreshes(bus) == 2);
  assert(bus.sequences().front() == 0xF7);
  assert(bus.last(0x3C) == std::vector<uint8_t>{0x80});
#endif
  // Power-down after gray must park the analog rails; wake requires a clean base.
  bus.clear();
  bw[400] = 0;
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  d.copyGrayscaleLsb(bus, gray.data());
  d.copyGrayscaleMsb(bus, empty.data());
  d.displayGray(bus, bw.data(), true, nullptr, false);
#if FREEINK_SSD1677_TEXT_ROUTING
  assert(bus.sequences().back() == 0x03);
#endif
  d.deepSleep(bus);
  bus.clear();
  d.display(bus, bw.data(), nullptr, Mode::Fast, false);
#if FREEINK_SSD1677_TEXT_ROUTING
  assert(bus.sequences() == std::vector<uint8_t>{0xF7});
#endif
  // A BUSY timeout at the gray activation cannot commit or issue power-off.
  bus.clear();
  bw[500] = 0;
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  d.copyGrayscaleLsb(bus, gray.data());
  d.copyGrayscaleMsb(bus, empty.data());
  bus.failAt = bus.activation + 1;
  d.displayGray(bus, bw.data(), true, nullptr, false);
  assert(bus.isBusy() && !d.displayCommitted());
  const auto eventsAfterFailure = bus.events.size();
  d.cleanupGrayscaleBuffers(bus, bw.data());
  d.controllerIdle(bus);
  d.deepSleep(bus);
  assert(bus.events.size() == eventsAfterFailure + 1);  // deepSleep waits, but never writes
  bus.failAt = 0;
  bus.busy = false;
  bus.clear();
  d.displayGrayscaleBase(bus, bw.data(), Mode::Fast, false);
  d.copyGrayscaleLsb(bus, gray.data());
  d.copyGrayscaleMsb(bus, empty.data());
  d.displayGray(bus, bw.data(), false, nullptr, false);
  assert(d.displayCommitted());
#if FREEINK_SSD1677_TEXT_ROUTING
  assert(bus.sequences().front() == 0xF7);  // recovered page resynchronizes
#endif
  assert(allocationCalls == allocated);  // no per-page allocations
}
