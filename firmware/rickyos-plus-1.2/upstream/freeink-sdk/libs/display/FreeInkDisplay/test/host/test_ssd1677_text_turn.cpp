#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <vector>

#define private public
#include "driver/PaperMonoDriver.h"
#undef private
#include "driver/PaperMonoDriver.cpp"

using namespace freeink;
using Plane = std::array<uint8_t, 48000>;
enum class Ink { White, Gray, Black };

void setPixel(Plane& bw, Plane& gray, unsigned pixel, Ink ink) {
  const auto bit = static_cast<uint8_t>(0x80 >> (pixel % 8));
  bw[pixel / 8] |= bit;
  gray[pixel / 8] &= ~bit;
  switch (ink) {
    case Ink::White:
      break;
    case Ink::Gray:
      gray[pixel / 8] |= bit;
      break;
    case Ink::Black:
      bw[pixel / 8] &= ~bit;
      break;
  }
}

struct Paint {
  uint8_t control;
  std::vector<uint8_t> next, old, lut;
};
std::vector<Paint> paints(const EpdBus& bus) {
  Paint current{};
  std::vector<Paint> result;
  for (const auto& e : bus.events) {
    switch (e.cmd) {
      case 0x24:
        current.next = e.bytes;
        break;
      case 0x26:
        current.old = e.bytes;
        break;
      case 0x32:
        current.lut = e.bytes;
        break;
      case 0x22:
        current.control = e.bytes.at(0);
        break;
      case 0x20:
        if (current.control & 4) result.push_back(current);
        break;
      default:
        break;
    }
  }
  return result;
}
bool bit(const std::vector<uint8_t>& plane, unsigned pixel) { return plane.at(pixel / 8) & (0x80 >> (pixel % 8)); }

PaperMonoDriver* cancelDriver = nullptr;
void cancelDuringRefresh() { cancelDriver->abortPostRefresh(); }

int main() {
  EpdBus bus;
  PaperMonoDriver d;
  Plane bw, gray{}, empty{};
  bw.fill(0xff);
  d.begin(bus);
  const int allocations = allocationCalls;
  const auto stage = [&](RefreshMode mode = RefreshMode::Fast) {
    d.displayGrayscaleBase(bus, bw.data(), mode, false);
    d.copyGrayscaleLsb(bus, gray.data());
    d.copyGrayscaleMsb(bus, empty.data());
  };
  const auto finish = [&](bool off = false) { d.displayGray(bus, bw.data(), off, nullptr, false); };
  const auto page = [&](RefreshMode mode = RefreshMode::Fast) {
    stage(mode);
    finish();
  };
  // Establish a trustworthy source with the unchanged safe path. Each row of
  // three pixels starts W/G/B; the next page exercises all nine transitions.
  for (unsigned old = 0; old < 3; ++old)
    for (unsigned target = 0; target < 3; ++target) setPixel(bw, gray, old * 3 + target, static_cast<Ink>(old));
  page(RefreshMode::Half);
  assert(paints(bus).size() == 2);  // safe clear + combined first page
  bus.clear();
  for (unsigned old = 0; old < 3; ++old)
    for (unsigned target = 0; target < 3; ++target) setPixel(bw, gray, old * 3 + target, static_cast<Ink>(target));
  page();
  auto p = paints(bus);
  assert(p.size() == 1 && p[0].control == 0x0C);
  assert(bus.last(0x21) == (std::vector<uint8_t>{0, 0}));
  for (unsigned old = 0; old < 3; ++old) {
    for (unsigned target = 0; target < 3; ++target) {
      const auto pixel = old * 3 + target;
      const unsigned entry = bit(p[0].next, pixel) + 2 * bit(p[0].old, pixel);
      assert(entry == (target == 0 ? 1 : (target == 2 ? 3 : (old == target ? 0 : 2))));
    }
  }
  for (size_t i = 2; i < bw.size(); ++i) assert(p[0].next[i] == 0xff && p[0].old[i] == 0);
  const auto expected = p[0].lut;
  // Decode the actual uploaded waveform frame by frame, independently of the
  // driver's boundary sorting. Idle and VCOM never drive; independent white
  // and black extensions must not extend gray or shorten endpoint drive.
  constexpr unsigned whiteEnd = 24 + FREEINK_SSD1677_TEXT_WHITE_FRAMES;
  constexpr unsigned blackStart = 24 + FREEINK_SSD1677_TEXT_BLACK_DELAY;
  constexpr unsigned total = std::max(whiteEnd, blackStart + 32);
  unsigned whiteTicks = 0, grayWhiteTicks = 0, grayWeakTicks = 0, blackTicks = 0;
  unsigned frame = 0;
  for (unsigned group = 0; group < 10; ++group) {
    assert(expected[50 + group * 5 + 4] == 0);
    for (unsigned phase = 0; phase < 4; ++phase) {
      const unsigned duration = expected[50 + group * 5 + phase];
      for (unsigned tick = 0; tick < duration; ++tick, ++frame) {
        assert(frame < total);
        for (unsigned entry = 0; entry < 5; ++entry) {
          const unsigned vs = (expected[entry * 10 + group] >> ((3 - phase) * 2)) & 3;
          unsigned want = 0;
          if (entry == 1 && frame >= 24 && frame < whiteEnd) {
            want = 2;
            ++whiteTicks;
          }
          if (entry == 2 && frame < 56) {
            want = frame < 32 ? 2 : 3;
            if (frame < 32)
              ++grayWhiteTicks;
            else
              ++grayWeakTicks;
          }
          if (entry == 3 && frame >= blackStart && frame < blackStart + 32) {
            want = 1;
            ++blackTicks;
          }
          assert(vs == want);
        }
      }
    }
  }
  assert(frame == total);
  assert(whiteTicks == FREEINK_SSD1677_TEXT_WHITE_FRAMES);
  assert(grayWhiteTicks == 32 && grayWeakTicks == 24 && blackTicks == 32);
  for (size_t i = 100; i < 105; ++i) assert(expected[i] == 0x88);
  assert(d.displayCommitted() && d._panelHasGray);
  const auto& panel = combinedAa::calibration();
  assert(bus.last(0x03) == std::vector<uint8_t>{panel.voltages[0]});
  assert(bus.last(0x04) == std::vector<uint8_t>(panel.voltages + 1, panel.voltages + 4));
  assert(bus.last(0x2C) == std::vector<uint8_t>{panel.voltages[4]});
  assert(bus.last(0x3C) == std::vector<uint8_t>{panel.border});
  bus.clear();
  page();
  assert(paints(bus).empty());
  // Gray-only and black/white-only changes each need just one pixel activation.
  setPixel(bw, gray, 20, Ink::Gray);
  page();
  assert(paints(bus).size() == 1 && paints(bus)[0].control == 0x0C);
  bus.clear();
  gray.fill(0);
  page();
  assert(paints(bus).size() == 1 && paints(bus)[0].control == 0x0C && !d._panelHasGray);

  // A canceled staged target must not advance the source model.
  bus.clear();
  setPixel(bw, gray, 21, Ink::Black);
  stage();
  d.abortPostRefresh();
  finish();
  assert(paints(bus).empty() && !d.displayCommitted());
  // Cancellation discovered inside submission (after displayGray's guard)
  // must not record the undisplayed B/W target as the last committed page.
  stage();
  d.abortPostRefresh();
  assert(!d.commitPending(bus, true));
  assert(d._lastBw[21 / 8] & (0x80 >> (21 % 8)));
  assert(paints(bus).empty());
  // Once pixel drive starts, cancellation cannot discard the committed AA page.
  setPixel(bw, gray, 22, Ink::Gray);
  stage();
  cancelDriver = &d;
  bus.onRefreshComplete = cancelDuringRefresh;
  finish();
  bus.onRefreshComplete = nullptr;
  assert(paints(bus).size() == 1 && d.displayCommitted());

  // Rail-settle and pixel-drive failures invalidate the baseline and stop IO.
  for (int failure = 1; failure <= 2; ++failure) {
    d.controllerIdle(bus);
    bus.clear();
    setPixel(bw, gray, 30 + failure, Ink::Black);
    setPixel(bw, gray, 40 + failure, Ink::Gray);
    const Plane oldNonWhite = [&] {
      Plane v;
      std::copy_n(d._glassNonWhite, v.size(), v.begin());
      return v;
    }();
    const Plane oldBlack = [&] {
      Plane v;
      std::copy_n(d._glassBlack, v.size(), v.begin());
      return v;
    }();
    stage();
    bus.failAt = bus.activation + failure;
    finish(true);
    assert(bus.busy && !d.displayCommitted() && !d._lastBwValid && d._needsFull);
    assert(paints(bus).size() == static_cast<size_t>(failure - 1));
    assert(std::equal(oldNonWhite.begin(), oldNonWhite.end(), d._glassNonWhite));
    assert(std::equal(oldBlack.begin(), oldBlack.end(), d._glassBlack));
    const auto events = bus.events.size();
    d.cleanupGrayscaleBuffers(bus, bw.data());
    d.controllerIdle(bus);
    assert(bus.events.size() == events);
    bus.failAt = 0;
    bus.busy = false;
    bus.clear();
    page();
    assert(paints(bus).front().control == 0xF7 && d.displayCommitted());
  }
  // Idle wake retains glass; power-cut sleep, manual clean and unknown history do not.
  bus.clear();
  d.controllerIdle(bus);
  assert(bus.sequences().back() == combinedAa::calibration().powerOff);
  bus.clear();
  setPixel(bw, gray, 50, Ink::Black);
  setPixel(bw, gray, 51, Ink::Gray);
  page();
  assert(paints(bus).size() == 1 && paints(bus).front().control == 0x0C);
  bus.clear();
  page(RefreshMode::Half);
  assert(paints(bus).front().control == 0xF7);
  d.deepSleep(bus);
  bus.clear();
  page();
  assert(paints(bus).front().control == 0xF7);
  bus.clear();
  d.setBackgroundHint(true);
  setPixel(bw, gray, 60, Ink::Black);
  page();
  assert(paints(bus).size() == 1 && paints(bus)[0].lut != expected);
  d.setBackgroundHint(false);

  // Gray-only work after idle needs a successful rail-settle activation first.
  d.controllerIdle(bus);
  bus.clear();
  setPixel(bw, gray, 70, Ink::Gray);
  stage();
  bus.failAt = bus.activation + 1;
  finish(true);
  assert(bus.busy && !d.displayCommitted() && paints(bus).empty());
  assert(bus.sequences() == std::vector<uint8_t>{0xC0});
  bus.busy = false;
  bus.failAt = 0;
  bus.clear();
  page();
  d.controllerIdle(bus);
  bus.clear();
  setPixel(bw, gray, 71, Ink::Gray);
  page();
  assert(bus.sequences() == (std::vector<uint8_t>{0xC0, 0x0C}));

  // Cancellation during rail settling must still prevent pixel submission.
  d.controllerIdle(bus);
  bus.clear();
  setPixel(bw, gray, 72, Ink::Gray);
  stage();
  bus.onRefreshComplete = cancelDuringRefresh;
  finish();
  bus.onRefreshComplete = nullptr;
  assert(paints(bus).empty() && !d.displayCommitted());
  assert(!(d._glassNonWhite[72 / 8] & (0x80 >> (72 % 8))));
  bus.clear();
  page();
  assert(paints(bus).size() == 1 && d.displayCommitted());

  // Repeated forward/back changes exercise the committed source, including
  // the final byte of the panel. This is state/allocation coverage, not optics.
  for (unsigned n = 0; n < 100; ++n) {
    bus.clear();
    setPixel(bw, gray, 80, static_cast<Ink>(n % 3));
    setPixel(bw, gray, 383999, static_cast<Ink>((n + 1) % 3));
    page();
    const auto turns = paints(bus);
    assert(turns.size() == 1 && turns[0].control == 0x0C && turns[0].lut == expected);
    // Persistent status, guide and overlapping body pixels must retain black
    // drive on every changed page; unchanged gray must not be whitened again.
    for (const unsigned pixel : {2u, 5u, 8u}) {
      assert(bit(turns[0].next, pixel) && bit(turns[0].old, pixel));
    }
    assert(!bit(turns[0].next, 72) && !bit(turns[0].old, 72));
    for (size_t i = 0; i < bw.size(); ++i) {
      assert(d._glassNonWhite[i] == static_cast<uint8_t>(gray[i] | ~bw[i]));
      assert(d._glassBlack[i] == static_cast<uint8_t>(~bw[i] & ~gray[i]));
    }
    bus.clear();
    page();
    assert(paints(bus).empty());
  }
  assert(allocationCalls == allocations);
}
