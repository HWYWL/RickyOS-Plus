#include "BookCoverLoader.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "PanelTileCache.h"

namespace BookCoverLoader {
namespace {

enum class CachedCoverState { Missing, Ready, Terminal };

// Thumbnails known to have no usable cover in this session, so a page that returns
// does not reopen their markers. Bounded; cleared by forgetDrawnCovers().
constexpr size_t kMaxKnownCoverless = 64;
std::vector<std::string> knownCoverless;
std::mutex knownCoverlessLock;

bool isKnownCoverless(const std::string& path) {
  const std::lock_guard<std::mutex> guard(knownCoverlessLock);
  return std::find(knownCoverless.begin(), knownCoverless.end(), path) != knownCoverless.end();
}

void rememberCoverless(const std::string& path) {
  const std::lock_guard<std::mutex> guard(knownCoverlessLock);
  if (std::find(knownCoverless.begin(), knownCoverless.end(), path) != knownCoverless.end()) return;
  if (knownCoverless.size() >= kMaxKnownCoverless) knownCoverless.erase(knownCoverless.begin());
  knownCoverless.push_back(path);
}

CachedCoverState inspectCachedCover(const std::string& path, const bool emptyIsTerminal,
                                    const bool thumbnail = false) {
  if (!Storage.exists(path.c_str())) return CachedCoverState::Missing;

  {
    HalFile file;
    if (!Storage.openFileForRead("COVER", path, file)) return CachedCoverState::Terminal;
    if (file.fileSize() == 0 && emptyIsTerminal) return CachedCoverState::Terminal;

    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0 &&
        !(thumbnail && isOutdatedThumbnail(bitmap))) {
      return CachedCoverState::Ready;
    }
  }

  PanelTileCache::forget(path);
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("COVER", "Failed to remove invalid cover: %s", path.c_str());
    return CachedCoverState::Terminal;
  }
  return CachedCoverState::Missing;
}

template <typename Generate>
std::string ensureCachedCover(const std::string& path, const bool emptyIsTerminal, bool* generated,
                              Generate&& generate, const bool thumbnail = false) {
  if (generated) *generated = false;
  const CachedCoverState state = inspectCachedCover(path, emptyIsTerminal, thumbnail);
  if (state == CachedCoverState::Ready) return path;
  if (state == CachedCoverState::Terminal) return "";
  if (!generate()) {
    // Books without a usable cover would otherwise reload their metadata on every
    // request. An empty file is the terminal marker inspectCachedCover() honours;
    // clearing the reading cache removes it.
    if (emptyIsTerminal && !Storage.exists(path.c_str())) {
      HalFile marker;
      Storage.openFileForWrite("COVER", path, marker);
    }
    return "";
  }
  PanelTileCache::forget(path);
  if (inspectCachedCover(path, emptyIsTerminal, thumbnail) != CachedCoverState::Ready) return "";
  if (generated) *generated = true;
  return path;
}

}  // namespace

std::string ensureThumbnail(const std::string& bookPath, const int height, bool* generated) {
  if (generated) *generated = false;
  if (height <= 0) return "";
  if (const std::string known = thumbnailPath(bookPath, height); !known.empty() && isKnownCoverless(known)) return "";
  if (!Storage.exists(bookPath.c_str())) return "";

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    const std::string path = epub.getThumbBmpPath(height);
    std::string result = ensureCachedCover(path, !epub.hasCoverOverride(), generated, [&]() {
      // A book that declares no cover would otherwise pay a full load (seconds on a
      // large folder) only to find that out. The terminal marker then remembers it.
      bool declared = true;
      if (!epub.hasCoverOverride() && epub.probeCover(declared) && !declared) return false;
      if (!epub.load(true, true)) return false;
      epub.setupCacheDir();
      return epub.generateThumbBmp(height);
    }, /*thumbnail=*/true);
    if (result.empty()) rememberCoverless(path);
    return result;
  }


  return "";
}

std::string ensureFullCover(const std::string& bookPath, std::string* title, std::string* author, bool* generated) {
  if (title) title->clear();
  if (author) author->clear();
  if (generated) *generated = false;
  if (!Storage.exists(bookPath.c_str())) return "";

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    const std::string path = epub.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      if (!epub.load(true, true)) return false;
      epub.setupCacheDir();
      if (title) *title = epub.getTitle();
      if (author) *author = epub.getAuthor();
      return epub.generateCoverBmp();
    });
  }


  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    const std::string path = txt.getCoverBmpPath();
    return ensureCachedCover(path, false, generated, [&]() {
      if (!txt.load()) return false;
      txt.setupCacheDir();
      if (title) *title = txt.getTitle();
      return txt.generateCoverBmp();
    });
  }

  return "";
}

std::string thumbnailPath(const std::string& bookPath, const int height) {
  if (height <= 0 || !FsHelpers::hasEpubExtension(bookPath)) return "";
  return Epub(bookPath, "/.crosspoint").getThumbBmpPath(height);
}

bool isOutdatedThumbnail(const Bitmap& bitmap) {
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS || defined(SIMULATOR_DEVICE_READPICO)
  return bitmap.is1Bit();
#else
  (void)bitmap;
  return false;
#endif
}

bool canMakeThumbnail(const std::string& bookPath, const int height) {
  const std::string path = thumbnailPath(bookPath, height);
  return !path.empty() && !isKnownCoverless(path);
}

void forgetDrawnCovers() {
  {
    const std::lock_guard<std::mutex> guard(knownCoverlessLock);
    knownCoverless.clear();
  }
  PanelTileCache::clear();
}

}  // namespace BookCoverLoader
