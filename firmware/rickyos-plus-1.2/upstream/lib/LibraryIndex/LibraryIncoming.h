#pragma once

#include <cstddef>

// Books a transfer wrote since the shelf last folded them in.
//
// The browser upload knows each finished file; the USB drive only knows that the
// computer had the card, so it records "anything may have changed". Both just
// append to a small file (/.crosspoint/library-incoming.txt): no directory walk or
// metadata read runs in the transfer path. The shelf reads the record on entry and
// refreshes only the enrolled sources that cover these paths.
namespace library {

class LibraryIncoming {
 public:
  static constexpr size_t MAX_PATHS = 200;
  // One uploaded book. Beyond MAX_PATHS the record degrades to unknown changes.
  static bool add(const char* path);
  // The USB host had the card: every enrolled source may have changed.
  static bool markUnknownChanges();
  static bool pending();
  // Calls `visit(path, context)` for each recorded path; `unknownChanges` reports
  // whether a USB session (or an overflow) asked for every source.
  static bool read(void (*visit)(const char* path, void* context), void* context, bool& unknownChanges);
  static void clear();
};

}  // namespace library
