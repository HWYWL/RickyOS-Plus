#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Socket seam for compiling the production HttpDownloader. The configure and
// abort callbacks run in the SDK order: begin -> configure -> abort -> connect.
// This does not simulate certificates or claim to test the TLS handshake.
namespace test_network {
inline std::vector<bool> origins{true};
inline int fetches = 0, connections = 0, trusted = 0, insecure = 0;
inline bool downgrade = false;
inline void reset(std::vector<bool> attempts) {
  origins = std::move(attempts);
  fetches = connections = trusted = insecure = 0;
  downgrade = false;
}
}  // namespace test_network
namespace freeink {
struct SecureHttpClient {
  using AbortCallback = std::function<bool()>;
  void setTimeout(int) {}
  void setCACert(const char* root) {
    if (root) ++test_network::trusted;
  }
  void setInsecure() { ++test_network::insecure; }
  void setUserAgent(const std::string&) {}
  void setBasicAuth(const std::string&, const std::string&) {}
  void addHeader(const std::string&, const std::string&) {}
};
struct FetchOptions {
  bool redirectToHttp = false;
};
struct FetchSink {
  std::function<bool(const uint8_t*, size_t)> write;
  std::function<bool()> rewind;
  std::function<void(size_t, size_t)> progress;
};
struct FetchResult {
  int status = 200;
  size_t bytes = 0, total = 0;
  bool complete = false, stopped = false, aborted = false;
};
inline FetchResult fetchResumable(const std::string&, const FetchOptions& options,
                                  const std::function<void(SecureHttpClient&, bool)>& configure, const FetchSink& sink,
                                  const SecureHttpClient::AbortCallback& abort) {
  ++test_network::fetches;
  test_network::downgrade = options.redirectToHttp;
  FetchResult result;
  for (const bool sameOrigin : test_network::origins) {
    SecureHttpClient http;
    configure(http, sameOrigin);
    if (abort && abort()) {
      result.aborted = true;
      return result;
    }
    ++test_network::connections;
  }
  const uint8_t byte = 42;
  if (!sink.write(&byte, 1)) {
    result.stopped = true;
    return result;
  }
  result.bytes = result.total = 1;
  result.complete = true;
  return result;
}
}  // namespace freeink
