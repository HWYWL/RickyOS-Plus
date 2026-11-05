#include <BoardConfig.h>

#include <array>

#include "driver/PaperMonoDriver.h"
#include "driver/Ssd1677Driver.h"
#include "lut/Ssd1677Luts.h"

using namespace freeink;

int activations(const EpdBus& bus) {
  int count = 0;
  for (const auto& event : bus.events)
    if (event.cmd == 0x20) ++count;
  return count;
}

int main() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
  BoardConfig::ACTIVE.displayWidth = 800;
  BoardConfig::ACTIVE.displayHeight = 480;
  BoardConfig::ACTIVE.displaySpiHz = 40000000;
  auto& original = ssd1677Driver();
  auto& combined = paperMonoDriver();
  assert(combined.prepareBuffers());
  EpdBus bus;
  std::array<uint8_t, 48000> bw, lsb{}, msb{};
  bw.fill(0xFF);
  bw[0] = 0x7F;
  lsb[0] = 0x80;
  msb[0] = 0x40;

  // Image: the two intermediate shades stay in separate controller planes,
  // and the original Sticky LUT (not the three-tone LUT) drives them.
  original.begin(bus);
  original.display(bus, bw.data(), nullptr, RefreshMode::Full, false);
  original.copyGrayscaleLsb(bus, lsb.data());
  original.copyGrayscaleMsb(bus, msb.data());
  assert(bus.last(0x24) == std::vector<uint8_t>(lsb.begin(), lsb.end()));
  assert(bus.last(0x26) == std::vector<uint8_t>(msb.begin(), msb.end()));
  original.displayGray(bus, bw.data(), false, nullptr, false);
  assert(bus.last(0x32) == std::vector<uint8_t>(lut_grayscale_sticky, lut_grayscale_sticky + 105));
  original.cleanupGrayscaleBuffers(bus, bw.data());
  original.deepSleep(bus);

  // Text: a cold switch may clean first, but the next text page is one gray
  // activation with no separate black-and-white body update.
  combined.begin(bus);
  combined.displayGrayscaleBase(bus, bw.data(), RefreshMode::Full, false);
  combined.copyGrayscaleLsb(bus, lsb.data());
  combined.copyGrayscaleMsb(bus, msb.data());
  combined.displayGray(bus, bw.data(), false, nullptr, false);
  combined.cleanupGrayscaleBuffers(bus, bw.data());
  bus.clear();
  bw[1] = 0;
  combined.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
  assert(activations(bus) == 0);
  combined.copyGrayscaleLsb(bus, lsb.data());
  combined.copyGrayscaleMsb(bus, msb.data());
  combined.displayGray(bus, bw.data(), false, nullptr, false);
  assert(activations(bus) == 1);
  combined.cleanupGrayscaleBuffers(bus, bw.data());

  // A canceled text frame must never appear after switching back to images.
  bus.clear();
  combined.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
  combined.abortPostRefresh();
  combined.cleanupGrayscaleBuffers(bus, bw.data());
  assert(activations(bus) == 0);
  combined.deepSleep(bus);
  original.begin(bus);
  bus.clear();
  original.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  original.copyGrayscaleLsb(bus, lsb.data());
  original.copyGrayscaleMsb(bus, msb.data());
  original.displayGray(bus, bw.data(), false, nullptr, false);
  assert(bus.last(0x32) == std::vector<uint8_t>(lut_grayscale_sticky, lut_grayscale_sticky + 105));
}
