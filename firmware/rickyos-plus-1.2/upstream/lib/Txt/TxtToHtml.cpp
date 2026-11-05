#include "TxtToHtml.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef RICKYOS_PRODUCT
#include "MarkdownFormatter.h"
#endif

const char* TxtToHtml::cacheVersionTag(std::string_view filename) {
#ifdef RICKYOS_PRODUCT
  if (FsHelpers::hasMarkdownExtension(filename)) return "<!-- MD_CACHE_VERSION: 6 -->";
#endif
  return FsHelpers::hasMarkdownExtension(filename) ? "<!-- MD_CACHE_VERSION: 3 -->" : "<!-- TXT_CACHE_VERSION: 3 -->";
}

bool TxtToHtml::stream(std::string_view filename, void* readerCtx, int (*readFn)(void*, uint8_t*, size_t), Print& out) {
  return stream(filename, readerCtx, readFn, out, Options{});
}

bool TxtToHtml::stream(std::string_view filename, void* readerCtx, int (*readFn)(void*, uint8_t*, size_t), Print& out,
                       const Options& options) {
  if (options.error) *options.error = txt_encoding::ReadError::Io;
  if (!txt_encoding::isSupported(options.encoding)) {
    if (options.error) *options.error = txt_encoding::ReadError::InvalidData;
    return false;
  }
  constexpr size_t BUFFER_SIZE = 8192;
  // Two fixed 8 KiB buffers, allocated once per conversion; task stacks cannot hold them.
  auto inBuf = makeUniqueNoThrow<uint8_t[]>(BUFFER_SIZE);
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(BUFFER_SIZE);
  if (!inBuf || !outBuf) {
    if (options.error) *options.error = txt_encoding::ReadError::OutOfMemory;
    LOG_ERR("TXT", "OOM: TXT/MD HTML streaming buffers (%zu bytes)", BUFFER_SIZE * 2);
    return false;
  }
#ifdef RICKYOS_PRODUCT
  const bool markdown = FsHelpers::hasMarkdownExtension(filename);
  // MD splits the existing 8 KiB input allocation: 1 KiB read, 4 KiB line,
  // 3 KiB lookahead header. No new heap; overlong lines remain literal.
  const size_t readCapacity = markdown ? 1024 : BUFFER_SIZE;
#else
  constexpr size_t readCapacity = BUFFER_SIZE;
#endif

  size_t outPos = 0;
  bool outputOk = true;
  const auto flushOut = [&] {
    if (outPos) {
      outputOk = outputOk && out.write(outBuf.get(), outPos) == outPos;
      outPos = 0;
    }
  };
  const auto writeByte = [&](const uint8_t b) {
    outBuf[outPos++] = b;
    if (outPos == BUFFER_SIZE) flushOut();
  };
  const auto writeStr = [&](const std::string_view s) {
    for (const char c : s) writeByte(static_cast<uint8_t>(c));
  };
  const auto escaped = [&](const uint8_t b) {
    switch (b) {
      case '&':
        writeStr("&amp;");
        break;
      case '<':
        writeStr("&lt;");
        break;
      case '>':
        writeStr("&gt;");
        break;
      default:
        writeByte(b);
        break;
    }
  };

  const std::string title = FsHelpers::getFileNameWithoutExtension(filename);
  const auto writeHead = [&] {
    writeStr("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    writeStr(cacheVersionTag(filename));
    writeStr("\n<!DOCTYPE html>\n<html>\n<head><title>");
    for (const char c : title) escaped(static_cast<uint8_t>(c));
    writeStr("</title></head>\n<body>\n");
  };
  writeHead();

  size_t inputPos = 0;
  int inputSize = 0;
  uint32_t source = 0;
  uint32_t visible = 1;  // The body opens with one newline, counted by ChapterHtmlSlimParser.
  bool readError = false;
  bool ioReadError = false;
  const auto readByte = [&]() -> int {
    if (inputPos == static_cast<size_t>(inputSize)) {
      inputSize = readFn(readerCtx, inBuf.get(), readCapacity);
      inputPos = 0;
      if (inputSize <= 0) {
        readError = inputSize < 0;
        ioReadError = readError;
        inputSize = 0;
        return -1;
      }
      if (inputSize > static_cast<int>(readCapacity)) {
        readError = true;
        return -1;
      }
    }
    if (source == UINT32_MAX) {
      readError = true;
      return -1;
    }
    ++source;
    return inBuf[inputPos++];
  };
  const auto map = [&](const uint32_t pos, const uint8_t width, const uint8_t step) {
    if (options.map && !options.map(options.context, pos, visible, width, step)) outputOk = false;
  };
  // Close this document and open the next at `at`. The global visible count carries on;
  // the new document's own count restarts with its body newline (see beginPart).
  const auto startPart = [&](const uint32_t at) {
    writeStr("\n</body>\n</html>\n");
    flushOut();
    if (!outputOk) return;
    if (!options.beginPart(options.context, at, visible)) {
      outputOk = false;
      return;
    }
    writeHead();
  };
  const auto maySplit = [&](const bool chapter, const uint32_t at) {
#ifdef RICKYOS_PRODUCT
    if (markdown) return;
#endif
    if (options.splitHere && options.beginPart && options.splitHere(options.context, chapter, outPos)) startPart(at);
  };
#ifdef RICKYOS_PRODUCT
  struct MarkdownContext {
    const Options& options;
    uint32_t& visible;
    bool& ok;
    const decltype(writeStr)& write;
    const decltype(escaped)& escape;
  } markdownContext{options, visible, outputOk, writeStr, escaped};
  MarkdownFormatter formatter(
      {&markdownContext,
       [](void* opaque, std::string_view text) { static_cast<MarkdownContext*>(opaque)->write(text); },
       [](void* opaque, uint32_t source, const uint8_t* character, uint8_t width, uint8_t utf8Length, bool show,
          bool spaces) {
         auto& ctx = *static_cast<MarkdownContext*>(opaque);
         if (ctx.options.map && !ctx.options.map(ctx.options.context, source, ctx.visible, width, show ? 1 : 0))
           ctx.ok = false;
         if (!show) return;
         ++ctx.visible;
         if (spaces && character[0] == ' ')
           ctx.write("&#160;");
         else if (character[0] < 0x20 && character[0] != '\t')
           ctx.write(" ");
         else
           for (uint8_t i = 0; i < utf8Length; ++i) ctx.escape(character[i]);
       }},
      inBuf.get() + 1024, 4096, inBuf.get() + 5120, 3072, options.encoding == txt_encoding::Encoding::Gbk,
      txt_encoding::isUtf16(options.encoding));
#endif

  bool atLineStart = true;
  uint32_t pendingSpaces = 0;
  uint32_t pendingStart = 0;
  uint8_t pendingWidth = 1;
  const auto finishSpaces = [&](const bool keep) {
    if (!pendingSpaces) return;
    map(pendingStart, pendingWidth, keep ? 1 : 0);
    if (keep) {
      for (uint32_t i = 1; i < pendingSpaces; ++i) writeStr("&#160;");
      writeByte(' ');
      visible += pendingSpaces;
    }
    pendingSpaces = 0;
  };

  while (outputOk) {
    const uint32_t begin = source;
    int first = readByte();
    if (first < 0) break;
    uint8_t character[8] = {static_cast<uint8_t>(first)};
    uint8_t width = 1;
    size_t utf8Length = 1;
    if (txt_encoding::isUtf16(options.encoding)) {
      txt_encoding::Utf16Decoder decoder(options.encoding);
      uint32_t cp = 0;
      for (;;) {
        const auto status = decoder.push(static_cast<uint8_t>(first), cp);
        if (status == txt_encoding::Utf16Decoder::Status::Ready) break;
        if (status == txt_encoding::Utf16Decoder::Status::Invalid || (first = readByte()) < 0) {
          readError = true;
          break;
        }
      }
      if (readError) break;
      width = static_cast<uint8_t>(source - begin);
      utf8Length = txt_encoding::encodeUtf8(cp, character);
      first = character[0];  // Control handling below uses decoded ASCII, not a source byte.
    } else if (first >= 0x80) {
      if (options.encoding == txt_encoding::Encoding::Gbk) {
        const int second = readByte();
        if (second < 0) {
          readError = true;
          break;
        }
        character[1] = static_cast<uint8_t>(second);
        width = 2;
        const auto converted = txt_encoding::transcodeGbkInPlace(character, 2, sizeof(character), true);
        if (converted.rawLength != 2 || converted.utf8Length == 0) {
          readError = true;
          break;
        }
        utf8Length = converted.utf8Length;
      } else {
        width = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC2 ? 2 : 0;
        if (width == 0 || first > 0xF4) {
          readError = true;
          break;
        }
        for (uint8_t i = 1; i < width; ++i) {
          const int next = readByte();
          if (next < 0 || (next & 0xC0) != 0x80) {
            readError = true;
            break;
          }
          character[i] = static_cast<uint8_t>(next);
        }
        if (readError) break;
        if (txt_encoding::detect(character, width, true) != txt_encoding::Encoding::Utf8) {
          readError = true;
          break;
        }
        utf8Length = width;
      }
    }

    if (options.chapterOffset && options.chapterOffset(options.context) == begin) {
#ifdef RICKYOS_PRODUCT
      if (markdown) formatter.beforeChapterAnchor();
#endif
      finishSpaces(true);
      maySplit(true, begin);
      if (!outputOk) break;
      char anchor[48];
      snprintf(anchor, sizeof(anchor), "<div id=\"txt-%u\"></div>", static_cast<unsigned>(begin));
      writeStr(anchor);
      if (!options.nextChapter(options.context)) {
        outputOk = false;
        break;
      }
    }
    if (begin == 0 && utf8Length == 3 && character[0] == 0xEF && character[1] == 0xBB && character[2] == 0xBF) {
      map(begin, width, 0);
      continue;
    }
#ifdef RICKYOS_PRODUCT
    if (markdown) {
      formatter.add(begin, character, width, static_cast<uint8_t>(utf8Length));
      continue;
    }
#endif
    if (first == '\r') {
      finishSpaces(false);
      map(begin, width, 0);
      continue;
    }
    if (first == '\n') {
      finishSpaces(false);
      map(begin, width, 0);
      writeStr("<br />");
      atLineStart = true;
      maySplit(false, source);
      continue;
    }
    if (first == ' ' && !atLineStart) {
      if (!pendingSpaces) {
        pendingStart = begin;
        pendingWidth = width;
      }
      ++pendingSpaces;
      continue;
    }
    finishSpaces(true);
    map(begin, width, 1);
    ++visible;
    if (first == ' ')
      writeStr("&#160;");
    else if (first < 0x20 && first != '\t')
      writeByte(' ');
    else
      for (size_t i = 0; i < utf8Length; ++i) escaped(character[i]);
    if (first != ' ') atLineStart = false;
  }
#ifdef RICKYOS_PRODUCT
  if (markdown && !readError && outputOk) formatter.finish();
#endif
  finishSpaces(false);
  map(source, 0, 0);  // EOF sentinel; no footer whitespace belongs to the source file.
  if (readError || !outputOk) {
    if (options.error)
      *options.error = readError ? (ioReadError ? txt_encoding::ReadError::Io : txt_encoding::ReadError::InvalidData)
                                 : txt_encoding::ReadError::Io;
    LOG_ERR("TXT", "TXT/MD conversion failed: %.*s", static_cast<int>(filename.size()), filename.data());
    return false;
  }
  writeStr("\n</body>\n</html>\n");
  flushOut();
  if (options.error) *options.error = outputOk ? txt_encoding::ReadError::None : txt_encoding::ReadError::Io;
  return outputOk;
}

bool TxtToHtml::stream(std::string_view filename, std::string_view content, Print& out) {
  struct ViewReader {
    std::string_view text;
    size_t pos = 0;
  } reader{content};
  return stream(
      filename, &reader,
      [](void* ctx, uint8_t* buf, size_t capacity) -> int {
        auto& reader = *static_cast<ViewReader*>(ctx);
        const size_t count = std::min(capacity, reader.text.size() - reader.pos);
        memcpy(buf, reader.text.data() + reader.pos, count);
        reader.pos += count;
        return static_cast<int>(count);
      },
      out);
}
