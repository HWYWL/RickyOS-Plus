#include <ResumableFetch.h>

#include <cassert>
#include <cstdio>

#include "src/network/HttpDownloader.h"

int main() {
  constexpr const char* url = "https://chinoryunqin.github.io/RickyOS-site/ota.json";
  int bodies = 0;
  const auto sink = [&bodies](const uint8_t*, size_t) {
    ++bodies;
    return true;
  };
  test_network::reset({true});
  assert(!HttpDownloader::fetchVerifiedUrl("http://example.com/ota.json", sink, "root"));
  assert(!HttpDownloader::fetchVerifiedUrl(url, sink, nullptr));
  assert(test_network::fetches == 0 && bodies == 0);

  assert(HttpDownloader::fetchVerifiedUrl(url, sink, "root"));
  assert(test_network::connections == 1 && test_network::trusted == 1 && test_network::insecure == 0);
  assert(!test_network::downgrade && bodies == 1);

  bodies = 0;
  test_network::reset({true, true});
  assert(HttpDownloader::fetchVerifiedUrl(url, sink, "root"));
  assert(test_network::connections == 2 && test_network::trusted == 2 && bodies == 1);

  bodies = 0;
  test_network::reset({true, false});
  assert(!HttpDownloader::fetchVerifiedUrl(url, sink, "root"));
  assert(test_network::connections == 1 && test_network::trusted == 1 && bodies == 0);
  assert(test_network::insecure == 0 && !test_network::downgrade);

  // Ordinary downloads retain their previous behavior; no global trust change.
  test_network::reset({true, false});
  assert(HttpDownloader::fetchUrl(url, sink));
  assert(test_network::connections == 2 && test_network::insecure == 2 && test_network::trusted == 0);
  std::puts("HttpDownloaderTrustTest: passed");
}
