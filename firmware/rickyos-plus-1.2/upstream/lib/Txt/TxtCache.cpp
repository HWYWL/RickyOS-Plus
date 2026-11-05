#include <BufferedFile.h>
#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>

#include <cstring>
#include <limits>

#include "Txt.h"
#include "TxtProgress.h"
#include "TxtToHtml.h"
#ifdef RICKYOS_PRODUCT
#include "MarkdownFormatter.h"
#endif

namespace {
bool openChapters(Txt& txt, HalFile& chapters, txt_encoding::Encoding& encoding, uint32_t& count,
                  txt_encoding::ReadError* error = nullptr) {
  if (error) *error = txt_encoding::ReadError::Io;
  if (!txt.load()) return false;
  if (txt.openChapterIndex(chapters, encoding, count)) {
    if (error) *error = txt_encoding::ReadError::None;
    return true;
  }
  chapters.close();  // The invalid index must close before replacement/reopen.
  // One 8 KiB indexing workspace, released before allocating conversion buffers.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(8192);
  if (!scratch) {
    if (error) *error = txt_encoding::ReadError::OutOfMemory;
    LOG_ERR("TXT", "OOM: chapter-index workspace (8192 bytes)");
    return false;
  }
  if (!txt.buildChapterIndex(encoding, scratch.get(), 8192, count, error)) return false;
  if (error) *error = txt_encoding::ReadError::Io;
  const bool ok = txt.openChapterIndex(chapters, encoding, count);
  if (ok && error) *error = txt_encoding::ReadError::None;
  return ok;
}

struct ConversionIndex {
  Txt& txt;
  HalFile& chapters;
  HalFile& mapping;
  txt_progress::Header header;
  txt_chapter_index::Record chapter;
  uint32_t chapterCount = 0;
  uint32_t nextChapter = 0;
  uint8_t lastWidth = 255;
  uint8_t lastStep = 255;

  bool advance() {
    if (nextChapter == chapterCount) {
      chapter.sourceOffset = UINT32_MAX;
      return true;
    }
    return txt.readChapter(chapters, chapterCount, nextChapter++, chapter);
  }
};
}  // namespace

namespace {
constexpr char kPartsMagic[4] = {'T', 'X', 'P', '1'};
// Group short chapters until a part holds this much HTML; split a long chapter (or a book
// without chapters) at the first line break past the larger size. A part is a few dozen
// pages and lays out in about two seconds; every part is a file, and creating 800 of them
// for one 20 MB novel took most of its first open, so parts are not made smaller.
constexpr size_t kPartChapterBytes = 48 * 1024;
constexpr size_t kPartLineBytes = 96 * 1024;

// The conversion's output: one HTML file per part, and the parts index beside them.
struct PartWriter final : Print {
  std::string dir;
  HalFile file;
  HalFile& index;
  uint32_t count = 0;
  size_t bytes = 0;
  bool ok = true;
  PartWriter(std::string dir, HalFile& index) : dir(std::move(dir)), index(index) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (!ok) return 0;
    const size_t written = file.write(buffer, size);
    bytes += written;
    if (written != size) ok = false;
    return written;
  }
  // The next document's own visible count restarts at 1 with its body newline, at global
  // offset `visible`: local + (visible - 1) is global.
  bool open(const uint32_t source, const uint32_t visible) {
    if (file) {
      file.flush();
      ok = file.close() && ok;
    }
    const Txt::Part part{source, visible ? visible - 1 : 0};
    if (!ok || index.write(reinterpret_cast<const uint8_t*>(&part), sizeof(part)) != sizeof(part)) return ok = false;
    const std::string path = dir + "/" + std::to_string(count++) + ".html";
    if (!Storage.openFileForWrite("TXT", path, file)) return ok = false;
    bytes = 0;
    return true;
  }
  bool close() {
    if (file) {
      file.flush();
      ok = file.close() && ok;
    }
    return ok;
  }
};
}  // namespace

std::string Txt::partHref(const int part) { return "content-" + std::to_string(part) + ".html"; }

int Txt::partForHref(std::string_view href) {
  constexpr std::string_view prefix = "content-", suffix = ".html";
  if (href.size() <= prefix.size() + suffix.size() || href.substr(0, prefix.size()) != prefix ||
      href.substr(href.size() - suffix.size()) != suffix)
    return -1;
  int part = 0;
  for (const char c : href.substr(prefix.size(), href.size() - prefix.size() - suffix.size())) {
    if (c < '0' || c > '9' || part > 100000) return -1;
    part = part * 10 + (c - '0');
  }
  return part;
}

bool Txt::usesParts(std::string_view path) {
  return FsHelpers::hasTxtExtension(path) && !FsHelpers::hasMarkdownExtension(path);
}

int Txt::partCount(const std::string& cachePath) {
  HalFile file;
  char magic[4] = {};
  uint32_t count = 0;
  if (!Storage.openFileForRead("TXT", cachePath + "/txt-parts.bin", file) || file.read(magic, 4) != 4 ||
      memcmp(magic, kPartsMagic, 4) != 0 || file.read(reinterpret_cast<uint8_t*>(&count), 4) != 4 || count == 0 ||
      file.size() != 8 + static_cast<size_t>(count) * sizeof(Part))
    return 0;
  return static_cast<int>(count);
}

bool Txt::readPart(const std::string& cachePath, const int index, Part& out) {
  HalFile file;
  return index >= 0 && Storage.openFileForRead("TXT", cachePath + "/txt-parts.bin", file) && readPart(file, index, out);
}

bool Txt::readPart(HalFile& file, const int index, Part& out) {
  if (index < 0) return false;
  const size_t at = 8 + static_cast<size_t>(index) * sizeof(Part);
  return at + sizeof(Part) <= file.size() && file.seek(at) &&
         file.read(reinterpret_cast<uint8_t*>(&out), sizeof(Part)) == sizeof(Part);
}

int Txt::partForVisibleOffset(const std::string& cachePath, const uint32_t visibleOffset) {
  const int count = partCount(cachePath);
  HalFile file;
  if (count <= 0 || !Storage.openFileForRead("TXT", cachePath + "/txt-parts.bin", file)) return -1;
  // Bases rise with the parts: binary search, one 8-byte read per step.
  int low = 0, high = count - 1;
  while (low < high) {
    const int mid = (low + high + 1) / 2;
    Part part;
    if (!readPart(file, mid, part)) return -1;
    if (part.visibleBase <= visibleOffset)
      low = mid;
    else
      high = mid - 1;
  }
  return low;
}

bool Txt::convertTxtParts(const std::string& filepath, const std::string& cachePath, txt_encoding::ReadError* error) {
  if (error) *error = txt_encoding::ReadError::Io;
  Txt txt(filepath, "/.crosspoint");
  HalFile source, chapters;
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t chapterCount = 0;
  if (!openChapters(txt, chapters, encoding, chapterCount, error)) return false;
  if (error) *error = txt_encoding::ReadError::Io;
  const std::string htmlDir = cachePath + "/html";
  if (txt.getFileSize() >= UINT32_MAX || !Storage.openFileForRead("TXT", filepath, source)) return false;
  Storage.removeDir(htmlDir.c_str());  // stale parts of an earlier split must not survive
  if (!Storage.ensureDirectoryExists(htmlDir.c_str())) return false;
  const std::string mapPath = cachePath + "/txt-map.bin", mapTemporary = mapPath + ".tmp";
  const std::string partsPath = cachePath + "/txt-parts.bin", partsTemporary = partsPath + ".tmp";
  bool converted = false;
  uint32_t parts = 0;
  {
    HalFile mapping, index;
    if (!Storage.openFileForWrite("TXT", mapTemporary, mapping) ||
        !Storage.openFileForWrite("TXT", partsTemporary, index))
      return false;
    const uint32_t placeholder = 0;
    if (index.write(reinterpret_cast<const uint8_t*>(kPartsMagic), 4) != 4 ||
        index.write(reinterpret_cast<const uint8_t*>(&placeholder), 4) != 4)
      return false;
    ConversionIndex conversion{txt, chapters, mapping};
    conversion.chapterCount = chapterCount;
    conversion.header.sourceSize = static_cast<uint32_t>(txt.getFileSize());
    conversion.header.encoding = static_cast<uint8_t>(encoding);
    if (!conversion.advance() ||
        mapping.write(reinterpret_cast<const uint8_t*>(&conversion.header), sizeof(conversion.header)) !=
            sizeof(conversion.header))
      return false;
    PartWriter writer(htmlDir, index);
    if (!writer.open(0, 1)) return false;  // part 0: local and global offsets coincide
    struct Context {
      ConversionIndex& conversion;
      PartWriter& writer;
    } context{conversion, writer};
    TxtToHtml::Options options;
    options.encoding = encoding;
    options.error = error;
    options.context = &context;
    options.chapterOffset = [](void* c) { return static_cast<Context*>(c)->conversion.chapter.sourceOffset; };
    options.nextChapter = [](void* c) { return static_cast<Context*>(c)->conversion.advance(); };
    options.map = [](void* c, const uint32_t sourceOffset, const uint32_t visible, const uint8_t width,
                     const uint8_t step) {
      auto& index = static_cast<Context*>(c)->conversion;
      if (width == index.lastWidth && step == index.lastStep) return true;
      if (index.mapping.position() > UINT32_MAX - sizeof(txt_progress::Record)) return false;
      const txt_progress::Record record{sourceOffset, visible, width, step};
      if (index.mapping.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record))
        return false;
      ++index.header.count;
      index.lastWidth = width;
      index.lastStep = step;
      return true;
    };
    options.splitHere = [](void* c, const bool chapter, const size_t pending) {
      const size_t bytes = static_cast<Context*>(c)->writer.bytes + pending;
      return bytes >= (chapter ? kPartChapterBytes : kPartLineBytes);
    };
    options.beginPart = [](void* c, const uint32_t at, const uint32_t visible) {
      return static_cast<Context*>(c)->writer.open(at, visible);
    };
    converted = TxtToHtml::stream(
        filepath, &source,
        [](void* ctx, uint8_t* buf, size_t size) { return static_cast<HalFile*>(ctx)->read(buf, size); }, writer,
        options);
    converted = writer.close() && converted;
    parts = writer.count;
    if (converted) {
      if (error) *error = txt_encoding::ReadError::Io;
      converted = mapping.seek(0) &&
                  mapping.write(reinterpret_cast<const uint8_t*>(&conversion.header), sizeof(conversion.header)) ==
                      sizeof(conversion.header) &&
                  index.seek(4) && index.write(reinterpret_cast<const uint8_t*>(&parts), 4) == 4;
      mapping.flush();
      index.flush();
    }
  }
  if (!converted || !Storage.replaceFile(mapTemporary.c_str(), mapPath.c_str()) ||
      !Storage.replaceFile(partsTemporary.c_str(), partsPath.c_str())) {
    Storage.remove(mapTemporary.c_str());
    Storage.remove(partsTemporary.c_str());
    Storage.removeDir(htmlDir.c_str());
    return false;
  }
  LOG_INF("TXT", "Split into %u parts: %s", static_cast<unsigned>(parts), filepath.c_str());
  if (error) *error = txt_encoding::ReadError::None;
  return true;
}

bool Txt::isTxtOrMd(std::string_view path) {
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
}

std::string Txt::findCompanionCoverImage(const std::string& filepath) {
  std::string folder = FsHelpers::extractFolderPath(filepath);

  std::string baseName = FsHelpers::getFileNameWithoutExtension(filepath);
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) return coverPath;
  }

  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) return coverPath;
    }
  }

  return "";
}

bool Txt::convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight,
                                 bool cropped, bool originalThresholds) {
  if (!Storage.exists(imagePath.c_str())) return false;

  const bool isBmp = FsHelpers::hasBmpExtension(imagePath);
  const bool isJpg = FsHelpers::hasJpgExtension(imagePath);
  const bool isPng = FsHelpers::hasPngExtension(imagePath);
  if (!isBmp && !isJpg && !isPng) return false;

  HalFile src, dst;
  if (!Storage.openFileForRead("TXT", imagePath, src) || !Storage.openFileForWrite("TXT", destBmpPath, dst)) {
    return false;
  }

  bool success = false;
  if (isBmp) {
    uint8_t buf[128];
    int n;
    success = true;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
      if (dst.write(buf, n) != static_cast<size_t>(n)) {
        success = false;
        break;
      }
    }
    if (n < 0) {
      success = false;
    }
  } else if (thumbHeight > 0) {
    const int targetWidth = thumbHeight * 0.6;
    const int targetHeight = thumbHeight;
    if (isJpg) {
      success = JpegToBmpConverter::jpegFileToThumbBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileToThumbBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    }
  } else {
    if (isJpg) {
      success = JpegToBmpConverter::jpegFileToCoverBmpStream(src, dst, cropped, originalThresholds);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileToCoverBmpStream(src, dst, cropped, originalThresholds);
    }
  }

  src.close();
  dst.close();

  if (!success) {
    Storage.remove(destBmpPath.c_str());
  }

  return success;
}

bool Txt::streamTxtToHtml(const std::string& filepath, Print& out, const std::string& cachePath,
                          txt_encoding::ReadError* error) {
  if (error) *error = txt_encoding::ReadError::Io;
  Txt txt(filepath, "/.crosspoint");
  HalFile source, chapters;
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t chapterCount = 0;
  if (!openChapters(txt, chapters, encoding, chapterCount, error)) return false;
  if (error) *error = txt_encoding::ReadError::Io;
  if (txt.getFileSize() >= UINT32_MAX || !Storage.openFileForRead("TXT", filepath, source)) return false;
  if (error) *error = txt_encoding::ReadError::Io;
  const std::string mapPath = cachePath + "/txt-map.bin";
  const std::string temporary = mapPath + ".tmp";
  bool converted = false;
  {
    HalFile mapping;
    if (!Storage.openFileForWrite("TXT", temporary, mapping)) return false;
    ConversionIndex index{txt, chapters, mapping};
    index.chapterCount = chapterCount;
    index.header.sourceSize = static_cast<uint32_t>(txt.getFileSize());
    index.header.encoding = static_cast<uint8_t>(encoding);
    if (!index.advance() || mapping.write(&index.header, sizeof(index.header)) != sizeof(index.header)) return false;
    TxtToHtml::Options options;
    options.encoding = encoding;
    options.error = error;
    options.context = &index;
    options.chapterOffset = [](void* context) { return static_cast<ConversionIndex*>(context)->chapter.sourceOffset; };
    options.nextChapter = [](void* context) { return static_cast<ConversionIndex*>(context)->advance(); };
    options.map = [](void* context, const uint32_t source, const uint32_t visible, const uint8_t width,
                     const uint8_t step) {
      auto& index = *static_cast<ConversionIndex*>(context);
      if (width == index.lastWidth && step == index.lastStep) return true;
      // Run-length records keep long ASCII/GBK spans compact; alternating widths remain disk-backed.
      if (index.mapping.position() > UINT32_MAX - sizeof(txt_progress::Record)) return false;
      const txt_progress::Record record{source, visible, width, step};
      if (index.mapping.write(&record, sizeof(record)) != sizeof(record)) return false;
      ++index.header.count;
      index.lastWidth = width;
      index.lastStep = step;
      return true;
    };
    converted = TxtToHtml::stream(
        filepath, &source,
        [](void* ctx, uint8_t* buf, size_t size) { return static_cast<HalFile*>(ctx)->read(buf, size); }, out, options);
    if (converted) {
      if (error) *error = txt_encoding::ReadError::Io;
      converted = mapping.seek(0) && mapping.write(&index.header, sizeof(index.header)) == sizeof(index.header);
      mapping.flush();
    }
  }
  if (!converted || !Storage.replaceFile(temporary.c_str(), mapPath.c_str())) {
    Storage.remove(temporary.c_str());
    return false;
  }
  if (error) *error = txt_encoding::ReadError::None;
  return true;
}

namespace {
bool resolveSourcePosition(const std::string& filepath, const std::string& cachePath, uint32_t sourceOffset,
                           uint32_t& visibleOffset) {
  Txt txt(filepath, "/.crosspoint");
  if (!txt.load() || txt.getFileSize() >= UINT32_MAX || sourceOffset > txt.getFileSize()) return false;
  const std::string mapPath = cachePath + "/txt-map.bin";
  const std::string htmlPath = cachePath + "/html/0.html";
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (Storage.exists(htmlPath.c_str())) {
      HalFile mapping;
      if (Storage.openFileForRead("TXT", mapPath, mapping) &&
          txt_progress::resolve(mapping, txt.getFileSize(), sourceOffset, visibleOffset))
        return true;
    }
    if (attempt) break;
    if (Txt::usesParts(filepath)) {
      // The map is written with the parts (and is book-wide), so regenerate them together.
      if (!Txt::convertTxtParts(filepath, cachePath)) break;
      continue;
    }
    if (!Storage.ensureDirectoryExists((cachePath + "/html").c_str())) break;
    const std::string temporary = htmlPath + ".tmp";
    bool converted = false;
    {
      HalFile output;
      if (!Storage.openFileForWrite("TXT", temporary, output)) break;
      converted = Txt::streamTxtToHtml(filepath, output, cachePath);
      output.flush();
    }
    if (!converted || !Storage.replaceFile(temporary.c_str(), htmlPath.c_str())) {
      Storage.remove(temporary.c_str());
      break;
    }
  }
  return false;
}
}  // namespace

bool Txt::resolveChapterPosition(const std::string& filepath, const std::string& cachePath, std::string_view anchor,
                                 uint32_t& visibleOffset) {
  if (anchor.size() <= 4 || anchor.substr(0, 4) != "txt-") return false;
  uint64_t sourceOffset = 0;
  for (char c : anchor.substr(4)) {
    if (c < '0' || c > '9') return false;
    sourceOffset = sourceOffset * 10 + static_cast<unsigned>(c - '0');
    if (sourceOffset > UINT32_MAX) return false;
  }
  return resolveSourcePosition(filepath, cachePath, static_cast<uint32_t>(sourceOffset), visibleOffset);
}

txt_progress::LegacyResult Txt::restoreLegacyProgress(const std::string& filepath, const std::string& cachePath,
                                                      uint32_t& visibleOffset) {
  Txt txt(filepath, "/.crosspoint");
  if (!txt.load() || txt.getFileSize() >= UINT32_MAX) return txt_progress::LegacyResult::Failed;
  uint32_t sourceOffset = 0;
  const auto legacy = txt_progress::readLegacySource(txt.getCachePath().c_str(), txt.getFileSize(), sourceOffset);
  if (legacy != txt_progress::LegacyResult::Restored) return legacy;
  if (resolveSourcePosition(filepath, cachePath, sourceOffset, visibleOffset)) return legacy;
  LOG_ERR("TXT", "Resume failed; keeping old progress: %s", filepath.c_str());
  return txt_progress::LegacyResult::Failed;
}

txt_progress::LegacyResult Txt::restoreMarkdownProgress(const std::string& filepath, const std::string& cachePath,
                                                        uint32_t& visibleOffset) {
  if (!FsHelpers::hasMarkdownExtension(filepath)) return txt_progress::LegacyResult::Absent;
  Txt txt(filepath, "/.crosspoint");
  if (!txt.load() || txt.getFileSize() >= UINT32_MAX) return txt_progress::LegacyResult::Failed;
  uint32_t source = 0;
  const auto result = txt_progress::readReflowSource(cachePath.c_str(), txt.getFileSize(), source);
  if (result != txt_progress::LegacyResult::Restored) return result;
  return resolveSourcePosition(filepath, cachePath, source, visibleOffset) ? result
                                                                           : txt_progress::LegacyResult::Failed;
}

void Txt::invalidateCache(const std::string& cachePath) {
  Storage.remove((cachePath + "/txt-parts.bin").c_str());
  Storage.removeDir((cachePath + "/html").c_str());
  Storage.removeDir((cachePath + "/sections").c_str());
}

bool Txt::validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize) {
  char metadataVersion[4] = {};
  const std::string marker = cachePath + "/txt-cache-version";
  bool valid = Storage.readFileToBuffer(marker.c_str(), metadataVersion, sizeof(metadataVersion)) == 1 &&
               metadataVersion[0] ==
#ifdef RICKYOS_PRODUCT
                   (FsHelpers::hasMarkdownExtension(filepath) ? '6' : '4');
#else
                   '4';
#endif
  // A split book also needs its parts index (version 4 writes it before the marker).
  if (valid && usesParts(filepath) && partCount(cachePath) <= 0) valid = false;

  // 1. If html/0.html exists, check its embedded version comment
  const std::string htmlPath = cachePath + "/html/0.html";
  if (Storage.exists(htmlPath.c_str())) {
    HalFile htmlFile;
    if (Storage.openFileForRead("TXT", htmlPath, htmlFile)) {
      char header[80] = {0};
      const int bytesRead = htmlFile.read(header, sizeof(header) - 1);
      if (bytesRead > 0) {
        header[bytesRead] = '\0';
        if (strstr(header, TxtToHtml::cacheVersionTag(filepath)) == nullptr) {
          LOG_DBG("TXT", "Stale HTML cache: %s", htmlPath.c_str());
          valid = false;
        }
      } else {
        valid = false;
      }
    } else {
      valid = false;
    }
  }

  // 2. Check if source file size changed
  if (valid && cachedSize > 0) {
    HalFile rawFile;
    if (Storage.openFileForRead("TXT", filepath, rawFile)) {
      if (rawFile.fileSize64() != cachedSize) {
        LOG_DBG("TXT", "File size changed: %u != %u", static_cast<uint32_t>(rawFile.size()),
                static_cast<uint32_t>(cachedSize));
        valid = false;
      }
    } else {
      valid = false;
    }
  }

  if (!valid) {
    LOG_DBG("TXT", "Rebuild HTML/sections: %s", filepath.c_str());
    invalidateCache(cachePath);
  }

  return valid;
}

bool Txt::buildTxtCache(const std::string& filepath, const std::string& cachePath,
                        std::unique_ptr<BookMetadataCache>& bookMetadataCache) {
  LOG_DBG("TXT", "Build TXT/MD metadata: %s", filepath.c_str());

#ifdef RICKYOS_PRODUCT
  if (FsHelpers::hasMarkdownExtension(filepath)) {
    char version[4] = {};
    if (Storage.readFileToBuffer((cachePath + "/txt-cache-version").c_str(), version, sizeof(version)) == 1 &&
        (version[0] == '2' || version[0] == '3' || version[0] == '4' || version[0] == '5')) {
      Txt txt(filepath, "/.crosspoint");
      if (!txt.load() || txt.getFileSize() >= UINT32_MAX ||
          txt_progress::preserveReflowSource(cachePath.c_str(), txt.getFileSize()) ==
              txt_progress::LegacyResult::Failed) {
        LOG_ERR("TXT", "MD resume failed; keeping old progress/map");
        return false;
      }
    }
  }
#endif

  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  } else {
    invalidateCache(cachePath);
  }

  // Plain TXT converts up front: the spine is one item per part, so the count must be known
  // before it is written. Markdown stays one document, converted when its section builds.
  const bool split = usesParts(filepath);
  int parts = 1;
  if (split) {
    if (!convertTxtParts(filepath, cachePath)) return false;
    parts = partCount(cachePath);
    if (parts <= 0 || parts >= UINT16_MAX) return false;
  }

  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("TXT", "Metadata begin failed");
    return false;
  }

  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("TXT", "Begin content.opf failed");
    return false;
  }

  if (split) {
    for (int part = 0; part < parts; ++part) bookMetadataCache->createSpineEntry(partHref(part));
  } else {
    bookMetadataCache->createSpineEntry("content.html");
  }

  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("TXT", "End content.opf failed");
    return false;
  }

  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("TXT", "Begin TOC failed");
    return false;
  }

  const std::string title = FsHelpers::getFileNameWithoutExtension(filepath);
  bookMetadataCache->createTocEntry(title, split ? partHref(0) : std::string("content.html"), "", 0);
  Txt txt(filepath, "/.crosspoint");
  HalFile chapters;
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  uint32_t count = 0;
  if (!openChapters(txt, chapters, encoding, count) || count >= UINT16_MAX - 1) return false;
#ifdef RICKYOS_PRODUCT
  const bool markdown = FsHelpers::hasMarkdownExtension(filepath);
#endif
  // Chapters and parts both run in source order: walk the parts alongside the chapters.
  int chapterPart = 0;
  Part nextPart;
  HalFile partsFile;
  bool haveNext = split && chapterPart + 1 < parts &&
                  Storage.openFileForRead("TXT", cachePath + "/txt-parts.bin", partsFile) &&
                  readPart(partsFile, chapterPart + 1, nextPart);
  for (uint32_t chapterIndex = 0; chapterIndex < count; ++chapterIndex) {
    txt_chapter_index::Record chapter;
    if (!txt.readChapter(chapters, count, chapterIndex, chapter)) return false;
    char anchor[32];
    snprintf(anchor, sizeof(anchor), "txt-%u", static_cast<unsigned>(chapter.sourceOffset));
    uint8_t level = 0;
#ifdef RICKYOS_PRODUCT
    if (markdown) {
      const auto title = txt_chapter_index::markdownTitle(chapter.title, level);
      // inlineText reads and consumes forward; its output never grows the input.
      MarkdownFormatter::plainText(title, chapter.title);
    }
#endif
    while (haveNext && nextPart.sourceStart <= chapter.sourceOffset) {
      ++chapterPart;
      haveNext = chapterPart + 1 < parts && readPart(partsFile, chapterPart + 1, nextPart);
    }
    bookMetadataCache->createTocEntry(chapter.title, split ? partHref(chapterPart) : std::string("content.html"),
                                      anchor, level);
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("TXT", "End TOC failed");
    return false;
  }

  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("TXT", "Metadata end failed");
    return false;
  }

  BookMetadataCache::BookMetadata bookMetadata;
  bookMetadata.title = utf8ComposeNfc(title);
  bookMetadata.language = "en";

  std::string companionCover = findCompanionCoverImage(filepath);
  if (!companionCover.empty()) {
    bookMetadata.coverItemHref = companionCover;
  }

  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("TXT", "book.bin build failed");
    return false;
  }

  bookMetadataCache->cleanupTmpFiles();

  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache || !bookMetadataCache->load()) {
    LOG_ERR("TXT", "Cache reload failed");
    return false;
  }

  if (!Storage.writeFile((cachePath + "/txt-cache-version").c_str(),
#ifdef RICKYOS_PRODUCT
                         FsHelpers::hasMarkdownExtension(filepath) ? "6" :
#endif
                                                                   "4"))
    return false;
  return true;
}
