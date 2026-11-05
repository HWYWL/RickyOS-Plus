#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Forward-declared: fetchUrl() takes a Stream& by reference. On-device this name
// arrives transitively via the SdFat/Arduino chain in <HalStorage.h>; declaring
// it here keeps the header self-sufficient (the host build's SdFat shim doesn't
// pull Arduino's Stream in transitively).
class Stream;

/**
 * HTTP client utility for fetching content and downloading files.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Synchronous borrowed host, after URL parsing and before each socket request.
  using SourceCallback = void (*)(const char* host, void* context);
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    UNAUTHORIZED,  // 401/403: callers holding a refreshable credential can retry
  };

  // Pre-flight floor for the ordinary allocator used by TLS (including
  // registered PSRAM). This is a heuristic, not a reservation or OOM guarantee.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;
  static bool hasMemoryForTls();

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  // Owned firmware delivery: verify the CA chain and hostname, no insecure fallback.
  static bool fetchVerifiedUrl(const std::string& url, const DataCallback& onData, const char* rootCa);

  using Header = std::pair<std::string, std::string>;

  /**
   * Download a file to the SD card with optional credentials. `headers` are
   * added to the request (e.g. a Bearer Authorization), alongside any Basic
   * auth derived from username/password.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::vector<Header>& headers = {}, bool downgradeRedirectsToHttp = false,
                                      const std::function<void()>& pollInput = nullptr,
                                      SourceCallback onSource = nullptr, void* sourceContext = nullptr);

  // Small catalog indexes (font and picture lists) come from one of several mirrors.
  // Socket waits and all TLS retries, redirects and body resumes share this budget.
  // Abort polling stops a stalled route instead of restarting the budget per stage.
  // A blocking DNS/TCP operation may return after the budget before polling resumes.
#if FREEINK_DEVICE_METALIO_PLUS
  // S31 can stall the first socket after association. Allow one fresh socket
  // and its handshake/body within a single bounded catalog request.
  static constexpr uint32_t INDEX_TIMEOUT_MS = 30000;
#else
  static constexpr uint32_t INDEX_TIMEOUT_MS = 12000;
#endif
  static DownloadError downloadIndex(const std::string& url, const std::string& destPath);

 private:
  static DownloadError downloadToFileWithin(const std::string& url, const std::string& destPath, uint32_t timeoutMs,
                                            ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
                                            const std::string& username = "", const std::string& password = "",
                                            const std::vector<Header>& headers = {},
                                            bool downgradeRedirectsToHttp = false,
                                            const std::function<void()>& pollInput = nullptr,
                                            uint32_t totalBudgetMs = 0,
                                            SourceCallback onSource = nullptr, void* sourceContext = nullptr);
};
