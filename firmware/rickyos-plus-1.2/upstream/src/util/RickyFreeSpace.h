#pragma once

#include <cstdint>

namespace RickyFreeSpace {
// Filesystem capacity and free bytes, like Storage.getSpace(), but the free count
// reads the allocation table a slice at a time and releases the SD lock between
// slices, so other pages keep using the card while a large card is measured.
// Seconds on a big FAT32 card: call it from a background task, never the UI loop.
bool measure(uint64_t& totalBytes, uint64_t& freeBytes);
}  // namespace RickyFreeSpace

