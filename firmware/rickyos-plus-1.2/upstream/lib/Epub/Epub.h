#pragma once
#include <CancelCheck.h>
#include <ContentProtection.h>
#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;
namespace txt_encoding {
enum class ReadError : uint8_t;
}

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // MOBI/KF8 use a disposable archive; filepath remains the progress identity.
#if defined(RICKYOS_KINDLE_IMPORT)
  std::string archivePath;
  const std::string& archiveFile() const { return archivePath.empty() ? filepath : archivePath; }
#else
  const std::string& archiveFile() const { return filepath; }
#endif
  bool prepareArchive();
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // Optional encrypted-entry accessor. Entries are decoded in memory and stay
  // encrypted at rest. Null when the accessor is not needed or unavailable.
  std::unique_ptr<freeink::content::ContentDecryptor> decryptor;
  // User-presentable reason the encrypted-entry accessor could not be opened.
  std::string protectionError;

  bool findContentOpfFile(std::string* contentOpfFile, ZipFile* sharedZip = nullptr) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       bool metadataOnly = false, ZipFile* sharedZip = nullptr);
  bool generateThumbBmpForCover(int height, const std::string& coverImageHref) const;
  bool openProtection();
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  CssParser::ParseResult parseCssFiles(CssParser::CacheStatus existingCacheStatus, int progressFrom = 0,
                                       int progressTo = 0) const;

 public:
  // Optional progress of load() (0-100) while it builds missing caches, reported
  // between steps on the calling task. Reported values never decrease.
  using LoadProgressFn = void (*)(void* context, int percent);
  void setLoadProgress(LoadProgressFn fn, void* context) {
    loadProgress = fn;
    loadProgressContext = context;
  }

 private:
  LoadProgressFn loadProgress = nullptr;
  void* loadProgressContext = nullptr;
  void reportLoad(const int percent) const {
    if (loadProgress) loadProgress(loadProgressContext, percent);
  }

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  // `openedArchive`, when given and open, is this EPUB already opened by the caller
  // (a library walk); it is consumed instead of opening the file again by name.
  bool loadMetadata(std::string& title, std::string& author, HalFile* openedArchive = nullptr);
  // Whether the package declares a cover image (metadata, EPUB 3 property or guide
  // page), read from the OPF alone: no reader caches are built or written. Returns
  // false when the OPF cannot be read, leaving `declared` untouched.
  bool probeCover(bool& declared);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  // Empty unless the encrypted-entry accessor failed to open.
  const std::string& getProtectionError() const { return protectionError; }
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // The cover image's href inside the book ("" when there is none or no cache is loaded).
  const std::string& getCoverItemHref() const;
  std::string getCoverOverridePath() const;
  bool hasCoverOverride() const;
  std::string getCoverBmpPath(bool cropped = false, bool originalThresholds = false) const;
  bool generateCoverBmp(bool cropped = false, bool originalThresholds = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  // Locate the cover without building spine, TOC, or reading caches.
  bool generateThumbBmpFromSource(int height);
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                                size_t psramChunkSize = 0, CancelCheck cancellation = {},
                                txt_encoding::ReadError* textError = nullptr) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath, CancelCheck cancellation = {}) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  // A plain TXT is split into parts (see Txt::Part): the part holding a global visible
  // offset (-1 when the book is not split), and the global offset where a part starts.
  int txtPartForVisibleOffset(uint32_t visibleOffset) const;
  uint32_t txtVisibleBase(int spineIndex) const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
