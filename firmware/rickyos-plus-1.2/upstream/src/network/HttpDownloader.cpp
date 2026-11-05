#include "HttpDownloader.h"

#include <Arduino.h>
#include <HalMemory.h>
#include <HalSystem.h>
#include <Logging.h>
#include <ResumableFetch.h>

#include <algorithm>
#include <functional>
#include <string>

#include "WifiPowerSaveGuard.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }

namespace {
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room.
constexpr uint32_t HTTP_TIMEOUT_MS = 60000;

// scheme://host[:port] of a URL for logs: paths, queries and userinfo can carry tokens.
std::string originOf(const std::string& url) {
  const size_t hostStart = url.find("://");
  if (hostStart == std::string::npos) return {};
  std::string origin = url.substr(0, url.find_first_of("/?#", hostStart + 3));
  const size_t at = origin.rfind('@');
  if (at != std::string::npos && at > hostStart) origin.erase(hostStart + 3, at + 1 - (hostStart + 3));
  return origin;
}

// All HTTP(S) fetches go through wolfSSL (the firmware's only TLS stack: it
// speaks TLS 1.3 and reads large bodies reliably). Plain-http URLs still use a
// WiFiClient here, so this is safe for non-TLS targets too. A body cut short
// mid-transfer resumes with a Range request (see ResumableFetch.h).
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password,
                                           const std::vector<HttpDownloader::Header>& headers,
                                           const freeink::FetchSink& sink, const bool* cancelFlag = nullptr,
                                           size_t* bytesOut = nullptr, const bool downgradeRedirectsToHttp = false,
                                           const char* rootCa = nullptr, const uint32_t timeoutMs = HTTP_TIMEOUT_MS,
                                           const std::function<void()>& pollInput = nullptr,
                                           const uint32_t totalBudgetMs = 0,
                                           HttpDownloader::SourceCallback onSource = nullptr,
                                           void* sourceContext = nullptr) {
  char userAgent[80];
  const int length =
      snprintf(userAgent, sizeof(userAgent), "CrossMux-%s-" CROSSPOINT_VERSION, HalSystem::getDeviceModel());
  if (length < 0 || static_cast<size_t>(length) >= sizeof(userAgent)) return HttpDownloader::HTTP_ERROR;
  WifiPowerSaveGuard psGuard;
  freeink::FetchOptions options;
  options.redirectToHttp = downgradeRedirectsToHttp;
  bool rejectedRedirect = false;
  uint32_t lastInputPoll = millis() - 20;
  const unsigned long started = millis();
  const freeink::FetchResult result = freeink::fetchResumable(
      url, options,
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
        // ResumableFetch follows redirects itself, outside the HTTP client's
        // downgrade guard. A verified firmware request must never leave its
        // original HTTPS origin. Abort before the redirected socket connects.
        if (rootCa && !sameOrigin) {
          rejectedRedirect = true;
          return;
        }
        if (onSource) onSource(http.getHost().c_str(), sourceContext);
        const uint32_t elapsed = static_cast<uint32_t>(millis() - started);
        const uint32_t remaining = totalBudgetMs > elapsed ? totalBudgetMs - elapsed : 1;
        http.setTimeout(totalBudgetMs == 0 ? timeoutMs : std::min(timeoutMs, remaining));
        if (rootCa)
          http.setCACert(rootCa);
        else
          http.setInsecure();
        // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
        // append a second User-Agent header, which strict servers reject (aiohttp
        // answers 400 "Duplicate 'User-Agent' header found").
        http.setUserAgent(userAgent);
        // Credentials and caller headers stay with the starting origin; a
        // redirect elsewhere (or to plain http) gets neither.
        if (sameOrigin) {
          if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);
          for (const auto& h : headers) http.addHeader(h.first, h.second);
        }
        LOG_DBG("HTTP", "wolfSSL GET: %s (heap %u, max block %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
      },
      sink, [&rejectedRedirect, cancelFlag, &pollInput, &lastInputPoll, started, totalBudgetMs] {
        // Poll even when no body bytes arrive (headers/TLS/retry waits).
        const uint32_t now = millis();
        if (pollInput && now - lastInputPoll >= 20) {
          lastInputPoll = now;
          pollInput();
        }
        return rejectedRedirect || (cancelFlag && *cancelFlag) ||
               (totalBudgetMs != 0 && static_cast<uint32_t>(now - started) >= totalBudgetMs);
      });
  if (bytesOut) *bytesOut = result.bytes;
  // Where a slow download spends its time: connecting to a dead route shows as a long
  // time with no bytes, a slow route as a low rate. Origin only: some URLs carry tokens.
  const unsigned long elapsed = millis() - started;
  LOG_INF("HTTP", "%zu B in %lu ms (%lu KB/s), status %d: %s", result.bytes, elapsed,
          elapsed > 0 ? static_cast<unsigned long>(result.bytes / elapsed) : 0UL, result.status, originOf(url).c_str());

  if (result.aborted) {
    // A catalog route exhausting its shared budget should fall through to the next
    // mirror; preserve ABORTED for a user cancellation or rejected verified redirect.
    if (totalBudgetMs != 0 && elapsed >= totalBudgetMs && !rejectedRedirect && !(cancelFlag && *cancelFlag)) {
      LOG_ERR("HTTP", "Index route budget exhausted: %s", originOf(url).c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::ABORTED;
  }
  if (result.stopped) return HttpDownloader::FILE_ERROR;
  if (result.status == 401 || result.status == 403) {
    LOG_ERR("HTTP", "wolfSSL request unauthorized: status %d: %s", result.status, url.c_str());
    return HttpDownloader::UNAUTHORIZED;
  }
  if (result.status < 200 || result.status >= 300) {
    LOG_ERR("HTTP", "wolfSSL request failed: status %d: %s", result.status, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (!result.complete) {
    LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", result.bytes, result.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

}  // namespace

bool HttpDownloader::hasMemoryForTls() {
  const auto available = HalMemory::getDefaultHeap();
  if (available.freeBytes >= MIN_TLS_FREE_HEAP && available.largestBlockBytes >= MIN_TLS_MAX_ALLOC) return true;

  const auto internal = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  LOG_ERR("HTTP",
          "TLS preflight rejected: default free=%zu largest=%zu, internal free=%zu largest=%zu, "
          "PSRAM free=%zu largest=%zu",
          available.freeBytes, available.largestBlockBytes, internal.freeBytes, internal.largestBlockBytes,
          psram.freeBytes, psram.largestBlockBytes);
  return false;
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  freeink::FetchSink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, {}, sink) == OK;
}

bool HttpDownloader::fetchVerifiedUrl(const std::string& url, const DataCallback& onData, const char* rootCa) {
  if (!rootCa || !url.starts_with("https://")) return false;
  freeink::FetchSink sink;
  sink.write = onData;
  return runGetSecure(url, "", "", {}, sink, nullptr, nullptr, false, rootCa) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers,
                                                             bool downgradeRedirectsToHttp,
                                                             const std::function<void()>& pollInput,
                                                             SourceCallback onSource, void* sourceContext) {
  return downloadToFileWithin(url, destPath, HTTP_TIMEOUT_MS, progress, cancelFlag, username, password, headers,
                              downgradeRedirectsToHttp, pollInput, 0, onSource, sourceContext);
}

HttpDownloader::DownloadError HttpDownloader::downloadIndex(const std::string& url, const std::string& destPath) {
  return downloadToFileWithin(url, destPath, INDEX_TIMEOUT_MS, nullptr, nullptr, "", "", {}, false, nullptr,
                              INDEX_TIMEOUT_MS);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFileWithin(const std::string& url, const std::string& destPath,
                                                                   const uint32_t timeoutMs, ProgressCallback progress,
                                                                   const bool* cancelFlag, const std::string& username,
                                                                   const std::string& password,
                                                                   const std::vector<Header>& headers,
                                                                   const bool downgradeRedirectsToHttp,
                                                             const std::function<void()>& pollInput,
                                                             const uint32_t totalBudgetMs,
                                                             SourceCallback onSource, void* sourceContext) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  // Stage in <dest>.part: a failed or cancelled download never replaces an
  // existing copy, and a partial file never sits under the real name.
  const std::string partPath = destPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  freeink::FetchSink sink;
#if FREEINK_DEVICE_METALIO_PLUS
  // Per-transfer counters stay on the stack; no per-chunk allocation or logs.
  struct { uint64_t writeUs = 0, progressUs = 0; size_t calls = 0; } timing;
  sink.write = [&file, &timing](const uint8_t* data, size_t len) {
    const uint32_t started = micros();
    const bool ok = file.write(data, len) == len;
    timing.writeUs += static_cast<uint32_t>(micros() - started);
    ++timing.calls;
    return ok;
  };
#else
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
#endif
  // Reopening for write truncates: the server restarted the body from byte 0.
  sink.rewind = [&file, &partPath] {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };
#if FREEINK_DEVICE_METALIO_PLUS
  if (progress) sink.progress = [&progress, &timing](size_t bytes, size_t total) {
    const uint32_t started = micros();
    progress(bytes, total);
    timing.progressUs += static_cast<uint32_t>(micros() - started);
  };
  const uint32_t transferStarted = millis();
#else
  sink.progress = progress;
#endif

  size_t downloaded = 0;
#if FREEINK_DEVICE_METALIO_PLUS
  const std::function<void()> timedInput = pollInput ? std::function<void()>([&pollInput, &timing] {
    const uint32_t started = micros();
    pollInput();
    timing.progressUs += static_cast<uint32_t>(micros() - started);
  }) : nullptr;
#endif
  const DownloadError result =
      runGetSecure(url, username, password, headers, sink, cancelFlag, &downloaded, downgradeRedirectsToHttp,
#if FREEINK_DEVICE_METALIO_PLUS
                   nullptr, timeoutMs, timedInput, totalBudgetMs, onSource, sourceContext);
#else
                   nullptr, timeoutMs, pollInput, totalBudgetMs, onSource, sourceContext);
#endif
#if FREEINK_DEVICE_METALIO_PLUS
  LOG_INF("HTTP", "Transfer timing: bytes=%zu totalMs=%lu writeMs=%llu inputMs=%llu writes=%zu result=%d",
          downloaded, (unsigned long)(millis() - transferStarted), (unsigned long long)(timing.writeUs / 1000),
          (unsigned long long)(timing.progressUs / 1000), timing.calls, result);
#endif
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove. A failed rewind leaves no open handle.
  if (file.isOpen()) file.close();

  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }
  if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move download into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return OK;
}
