#include <HalOtaSlot.h>
#include <HalStorage.h>
#include <SdCardFontCache.h>

#include <algorithm>
#include <array>
#include <cassert>

int main() {
  using namespace font_cache_test;
  using namespace SdCardFontCache;
  constexpr auto path = "/fonts/Serif/Serif_16.cpfont";
  const auto original = flash;
  const auto rejected = [&](Result expected) {
    assert(preflight(path) == expected);
    assert(preload(path) == expected);
    assert(writes == 0 && erases == 0 && flash == original);
  };

  // Eligibility is read-only, including exact-capacity fonts.
  assert(capacity() == 4096);
  for (size_t size : {4095u, 4096u}) {
    sourceSize = size;
    assert(preflight(path) == Result::Ok);
    assert(writes == 0 && erases == 0 && flash == original);
  }
  sourceSize = 4097;
  rejected(Result::TooLarge);

  // Respect both the actual partition size and the global payload ceiling.
  slotSize = 8 * 1024 * 1024;
  assert(capacity() == 6549504);
  sourceSize = capacity();
  assert(preflight(path) == Result::Ok);
  ++sourceSize;
  rejected(Result::TooLarge);
  slotSize = flash.size();
  sourceSize = 64;

  slotValid = false;
  rejected(Result::NotSafe);
  slotValid = true;
  slotSafe = false;
  rejected(Result::NotSafe);
  slotSafe = true;
  sourceExists = false;
  rejected(Result::InvalidFont);
  sourceExists = true;
  sourceHeader[8] = 0;
  rejected(Result::InvalidFont);
  sourceHeader[8] = 4;
  sourceSize = 63;
  rejected(Result::InvalidFont);
  sourceSize = 64;

  // Recheck at execution: the source/slot can change while the prompt is open.
  assert(preflight(path) == Result::Ok);
  sourceSize = 4097;
  assert(preload(path) == Result::TooLarge);
  sourceSize = 64;
  assert(preflight(path) == Result::Ok);
  sourceExists = false;
  assert(preload(path) == Result::InvalidFont);
  sourceExists = true;
  assert(preflight(path) == Result::Ok);
  slotSafe = false;
  assert(preload(path) == Result::NotSafe);
  slotSafe = true;
  assert(writes == 0 && erases == 0 && flash == original);

  assert(preload(path) == Result::Ok);
  assert(writes > 0 && erases > 0);
  const auto committed = flash;
  writes = erases = 0;
  assert(preflight(path) == Result::AlreadyCached);
  assert(preload(path) == Result::AlreadyCached);
  assert(writes == 0 && erases == 0 && flash == committed);

  // Rejecting a replacement must also leave an existing cache intact.
  sourceSize = 4097;
  assert(preflight(path) == Result::TooLarge);
  assert(preload(path) == Result::TooLarge);
  assert(writes == 0 && erases == 0 && flash == committed);

  // Vector fonts (TrueType/OpenType) are accelerated too: identified by their sfnt tag,
  // their head and size; a changed file is no longer the cached one.
  constexpr auto vectorPath = "/fonts/Sans/Sans.ttf";
  const auto cpfontHeader = sourceHeader;
  for (const auto tag : {std::array<uint8_t, 4>{0, 1, 0, 0}, std::array<uint8_t, 4>{'O', 'T', 'T', 'O'},
                         std::array<uint8_t, 4>{'t', 'r', 'u', 'e'}}) {
    sourceHeader.fill(0);
    std::copy(tag.begin(), tag.end(), sourceHeader.begin());
    sourceHeader[5] = 9;  // numTables: part of the identity, like any head byte
    sourceSize = 64;
    assert(preflight(vectorPath) == Result::Ok);
    assert(preload(vectorPath) == Result::Ok);
    size_t payload = 0;
    assert(isValidFor(vectorPath, &payload) && payload == 64);
    assert(preflight(vectorPath) == Result::AlreadyCached);
    sourceHeader[5] = 10;
    assert(!isValidFor(vectorPath) && preflight(vectorPath) == Result::Ok);
    sourceHeader[5] = 9;
    sourceSize = 65;
    assert(!isValidFor(vectorPath) && preflight(vectorPath) == Result::Ok);
    sourceSize = 4097;
    assert(preflight(vectorPath) == Result::TooLarge);
  }
  // Collections and anything else are not fonts the cache can hold.
  for (const auto tag : {std::array<uint8_t, 4>{'t', 't', 'c', 'f'}, std::array<uint8_t, 4>{'P', 'K', 3, 4}}) {
    sourceHeader.fill(0);
    std::copy(tag.begin(), tag.end(), sourceHeader.begin());
    sourceSize = 64;
    assert(preflight(vectorPath) == Result::InvalidFont);
  }
  sourceHeader = cpfontHeader;
}
