#pragma once

#include <Print.h>

#include <cstdint>
#include <string_view>

#include "TxtEncoding.h"

class TxtToHtml {
 public:
  struct Options {
    txt_encoding::Encoding encoding = txt_encoding::Encoding::Utf8;
    txt_encoding::ReadError* error = nullptr;
    void* context = nullptr;
    bool (*map)(void*, uint32_t source, uint32_t visible, uint8_t sourceWidth, uint8_t visibleStep) = nullptr;
    uint32_t (*chapterOffset)(void*) = nullptr;
    bool (*nextChapter)(void*) = nullptr;
    // Plain TXT only. Asked before a chapter anchor (chapter = true) and after each line
    // break whether to start a new HTML document; pendingBytes is what is buffered but not
    // yet written to `out`. On true the current document is closed, beginPart() switches
    // `out` to the next one (source and global visible offset where it starts), and that
    // document opens with the same head, so each part parses on its own.
    bool (*splitHere)(void*, bool chapter, size_t pendingBytes) = nullptr;
    bool (*beginPart)(void*, uint32_t source, uint32_t visible) = nullptr;
  };

  static const char* cacheVersionTag(std::string_view filename);
  static bool stream(std::string_view filename, void* readerCtx, int (*readFn)(void*, uint8_t*, size_t), Print& out);
  static bool stream(std::string_view filename, void* readerCtx, int (*readFn)(void*, uint8_t*, size_t), Print& out,
                     const Options& options);
  static bool stream(std::string_view filename, std::string_view content, Print& out);
};
