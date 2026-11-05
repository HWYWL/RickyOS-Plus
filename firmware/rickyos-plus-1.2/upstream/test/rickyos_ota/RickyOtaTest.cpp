#include <esp_ota_ops.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/network/FirmwareBoardTag.h"
#include "src/network/FirmwareFlasher.h"
#include "src/network/HttpDownloader.h"
#include "src/network/OtaUpdater.h"
#include "src/network/RickyOtaManifest.h"
#include "src/network/RickyOtaTrust.h"

namespace {
size_t checks = 0;
#define CHECK(...)         \
  do {                     \
    ++checks;              \
    assert((__VA_ARGS__)); \
  } while (0)
std::string response;
std::vector<uint8_t> image;
std::vector<uint8_t> written;
std::vector<std::string> urls;
esp_partition_t running{0x10000, 0x640000}, next{0x650000, 0x640000};
int begins = 0, ends = 0, switches = 0, aborts = 0;
int writeError = 0, endError = 0;
bool networkError = false, cutDownload = false, missingPartition = false, cosDown = false;
uint16_t runningChip = 9;
const std::string noUpdate =
    R"({"schema":1,"product":"RickyOS","board":"readpico","channel":"stable","status":"no_update"})";
const std::string offeredVersion = "1.1.1";

void reset() {
  response.clear();
  image.assign(4096, 0xff);
  written.clear();
  urls.clear();
  image[0] = 0xe9;
  image[1] = 1;
  image[12] = 9;
  image[13] = 0;
  const std::string identity = std::string("RickyOS\0", 8) + offeredVersion + '\0' + "CROSSPOINT-BOARD-V1:readpico;";
  std::memcpy(image.data() + 100, identity.data(), identity.size());
  next = {0x650000, 0x640000};
  begins = ends = switches = aborts = writeError = endError = 0;
  networkError = cutDownload = missingPartition = cosDown = false;
  runningChip = 9;
}
std::string sha(const std::vector<uint8_t>& bytes) {
  unsigned char digest[32];
  SHA256(bytes.data(), bytes.size(), digest);
  std::string text;
  for (unsigned char value : digest) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", value);
    text += pair;
  }
  return text;
}
std::string offer(const std::string& version = offeredVersion) {
  return "{\"schema\":1,\"product\":\"RickyOS\",\"board\":\"readpico\",\"channel\":\"stable\","
         "\"status\":\"update_available\",\"version\":\"" +
         version + "\",\"file\":\"firmware/RickyOS-test.bin\",\"bytes\":4096,\"sha256\":\"" + sha(image) +
         "\",\"approved\":true,\"hardwareAccepted\":true,\"chipId\":9,\"flashBytes\":16777216}";
}
bool parses(const std::string& text, size_t chunk = 1) {
  RickyOtaManifest parser;
  for (size_t offset = 0; offset < text.size(); offset += chunk) {
    const size_t count = std::min(chunk, text.size() - offset);
    if (!parser.feed(reinterpret_cast<const uint8_t*>(text.data() + offset), count)) return false;
  }
  return parser.valid();
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
  const size_t at = text.find(from);
  CHECK(at != std::string::npos);
  text.replace(at, from.size(), to);
  return text;
}
void checkBlocked() {
  CHECK(switches == 0);
  CHECK(ends == 0);
  CHECK(aborts >= 1);
}
OtaUpdater accepted() {
  response = offer();
  OtaUpdater updater;
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Nightly) == OtaUpdater::OK);
  CHECK(updater.isUpdateNewer());
  return updater;
}
}  // namespace

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
  return missingPartition ? nullptr : &next;
}
const esp_partition_t* esp_ota_get_running_partition() { return &running; }
esp_err_t esp_ota_begin(const esp_partition_t* partition, size_t size, esp_ota_handle_t* handle) {
  CHECK(partition->address != running.address);
  CHECK(size == 4096);
  ++begins;
  *handle = 1;
  return 0;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void* data, size_t size) {
  if (writeError) return writeError;
  const auto* bytes = static_cast<const uint8_t*>(data);
  written.insert(written.end(), bytes, bytes + size);
  return 0;
}
esp_err_t esp_ota_abort(esp_ota_handle_t) {
  ++aborts;
  return 0;
}
esp_err_t esp_ota_end(esp_ota_handle_t) {
  ++ends;
  return endError;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
  CHECK(partition == &next);
  ++switches;
  return 0;
}
uint16_t firmware_flash::runningPartitionChipId() { return runningChip; }
bool HttpDownloader::fetchVerifiedUrl(const std::string& url, const DataCallback& consume, const char* rootCa) {
  CHECK(rootCa && std::string_view(rootCa).starts_with("-----BEGIN CERTIFICATE-----"));
  // Two owned routes laid out alike, each over its own pinned root.
  const bool cos = url.starts_with(ricky_ota::COS_BASE);
  CHECK(cos || url.starts_with(ricky_ota::SITE_BASE));
  CHECK(std::string_view(rootCa) == (cos ? ricky_ota::COS_ROOT_CA : ricky_ota::ROOT_CA));
  urls.push_back(url);
  if (networkError || (cos && cosDown)) return false;
  const std::string path = url.substr(std::strlen(cos ? ricky_ota::COS_BASE : ricky_ota::SITE_BASE));
  if (path == "ota.json") {
    for (const auto& character : response) {
      if (!consume(reinterpret_cast<const uint8_t*>(&character), 1)) return false;
    }
    return true;
  }
  if (path == "ota-notes.txt") return true;
  CHECK(path == "firmware/RickyOS-test.bin");
  for (size_t i = 0; i < image.size(); i += 7) {
    if (cutDownload && i > 1000) return false;
    if (!consume(image.data() + i, std::min(size_t{7}, image.size() - i))) return false;
  }
  return true;
}

int main() {
  reset();
  for (size_t chunk : {1u, 2u, 7u, 4096u}) {
    CHECK(parses(noUpdate, chunk));
    CHECK(parses(offer(), chunk));
  }
  const auto good = offer();
  for (const auto& bad : {replace(good, "RickyOS\"", "CrossMux\""),
                          replace(good, "readpico", "x4pro"),
                          replace(good, "stable", "nightly"),
                          replace(good, "true", "false"),
                          replace(good, "\"schema\":1", "\"schema\":1,\"schema\":1"),
                          replace(good, "\"schema\":1,", "\"schema\":1 "),
                          replace(good, "\"schema\":1", "\"schema\" 1"),
                          good.substr(0, good.size() - 1),
                          good + "{}",
                          replace(good, ":4096", ":\"4096\""),
                          replace(good, ":4096", ":4096.0"),
                          replace(good, ":4096", ":-4096"),
                          replace(good, ":4096", ":04096"),
                          replace(good, ":4096", ":4294967296"),
                          replace(good, ":4096", ":6291456"),
                          replace(good, "firmware/RickyOS-test.bin", "https://crossmux.com/firmware.bin"),
                          replace(good, "firmware/RickyOS-test.bin", "firmware/../private.bin"),
                          replace(good, offeredVersion, offeredVersion + "-dev"),
                          replace(good, "16777216", "8388608"),
                          replace(good, "\"chipId\":9", "\"chipId\":5"),
                          good + std::string(2048, ' '),
                          replace(good, ":true", ":tru"),
                          replace(good, "\"product\":", "\"unknown\":"),
                          replace(good, "\"RickyOS\"", "null"),
                          replace(good, "}", ",}"),
                          replace(noUpdate, "}", ",\"bytes\":4096}")})
    CHECK(!parses(bad));
  CHECK(ricky_ota::newer("1.1.0-dev", offeredVersion));
  CHECK(ricky_ota::newer("1.1.0-dev", "1.1.0"));
  CHECK(ricky_ota::newer("1.1.9", "1.1.10"));
  CHECK(!ricky_ota::newer("1.2.0", offeredVersion));
  CHECK(!ricky_ota::newer(offeredVersion, offeredVersion));
  CHECK(!ricky_ota::newer("1.6.5-readpico-rc", offeredVersion));
  CHECK(!ricky_ota::newer("1.1.0", offeredVersion + "-dev"));
  // The pre-1.1.0 CrossMux-derived scheme is not a RickyOS version any more.
  CHECK(!ricky_ota::newer("1.0.0", "1.6.5-rickyos-pico.13"));
  ricky_ota::StringScanner scanner("1.1.1-rickyos-pico.111");
  const std::string scanBytes = "noise1.1.1.1-rickyos-pico.111suffix";
  for (const auto c : scanBytes) scanner.feed(reinterpret_cast<const uint8_t*>(&c), 1);
  CHECK(scanner.found());
  board_tag::Scanner board;
  const std::string tag = "CROSSPOINT-BOARD-V1:readpico;";
  for (const auto c : tag) board.feed(reinterpret_cast<const uint8_t*>(&c), 1);
  CHECK(board.found() && !board.mismatch());
  reset();
  OtaUpdater updater;
  response = noUpdate;
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) == OtaUpdater::NO_UPDATE);
  CHECK(!updater.isUpdateNewer());
  CHECK(updater.installUpdate() == OtaUpdater::UPDATE_OLDER_ERROR);
  CHECK(begins == 0);
  updater = accepted();
  response = noUpdate;
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Nightly) == OtaUpdater::NO_UPDATE);
  CHECK(!updater.isUpdateNewer());
  updater = accepted();
  response = "broken";
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) != OtaUpdater::OK);
  CHECK(!updater.isUpdateNewer());
  reset();
  updater = accepted();
  CHECK(updater.installUpdate() == OtaUpdater::OK);
  CHECK(written == image);
  CHECK(ends == 1 && switches == 1 && aborts == 0);
  for (int failure = 0; failure < 9; ++failure) {
    reset();
    if (failure == 1) std::memset(image.data() + 100, 'x', 8);  // right digest, wrong brand
    if (failure == 2) image[115] = 'x';                         // right digest, wrong embedded version
    if (failure == 3) {
      const size_t at = 100 + 8 + offeredVersion.size() + 1;
      std::memset(image.data() + at, 'x', tag.size());  // right digest, no board tag
    }
    if (failure == 4) {
      const auto wrongTag = std::string("CROSSPOINT-BOARD-V1:x4pro;");
      std::memcpy(image.data() + 300, wrongTag.data(), wrongTag.size());  // conflicting board
    }
    if (failure == 5) image[12] = 5;
    updater = accepted();
    if (failure == 0) image[300] ^= 1;  // changed bytes, original manifest hash
    if (failure == 6) image.pop_back();
    if (failure == 7) cutDownload = true;
    if (failure == 8) writeError = 1;
    CHECK(updater.installUpdate() != OtaUpdater::OK);
    checkBlocked();
  }
  reset();
  updater = accepted();
  image.push_back(0);
  CHECK(updater.installUpdate() != OtaUpdater::OK);
  checkBlocked();
  for (int failure = 0; failure < 3; ++failure) {
    reset();
    updater = accepted();
    if (failure == 0) next.address = running.address;
    if (failure == 1) next.size = 4096;
    if (failure == 2) missingPartition = true;
    CHECK(updater.installUpdate() == OtaUpdater::INTERNAL_UPDATE_ERROR);
    CHECK(begins == 0 && switches == 0);
  }
  reset();
  updater = accepted();
  endError = 1;
  CHECK(updater.installUpdate() == OtaUpdater::INTERNAL_UPDATE_ERROR);
  CHECK(switches == 0);
  reset();
  response = offer("1.0.9");
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) == OtaUpdater::OK);
  CHECK(!updater.isUpdateNewer());
  CHECK(updater.installUpdate() == OtaUpdater::UPDATE_OLDER_ERROR);
  CHECK(begins == 0);
  // The bucket answers first and serves the image; the site only when the bucket is down.
  static_assert(ricky_ota::COS_ENABLED);
  reset();
  response = offer();
  updater = OtaUpdater{};
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) == OtaUpdater::OK);
  CHECK(urls.size() == 2 && urls[0] == ricky_ota::COS_MANIFEST_URL && urls[1] == ricky_ota::COS_NOTES_URL);
  CHECK(updater.installUpdate() == OtaUpdater::OK);
  CHECK(urls.back() == std::string(ricky_ota::COS_BASE) + "firmware/RickyOS-test.bin" && written == image);
  reset();
  response = offer();
  cosDown = true;
  updater = OtaUpdater{};
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) == OtaUpdater::OK);
  CHECK(urls.size() == 3 && urls[0] == ricky_ota::COS_MANIFEST_URL && urls[1] == ricky_ota::MANIFEST_URL &&
        urls[2] == ricky_ota::NOTES_URL);
  CHECK(updater.installUpdate() == OtaUpdater::OK);
  CHECK(urls.back() == std::string(ricky_ota::SITE_BASE) + "firmware/RickyOS-test.bin" && written == image);
  // An answer from the bucket is final: its "no update" does not fall through to the site.
  reset();
  response = noUpdate;
  updater = OtaUpdater{};
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) == OtaUpdater::NO_UPDATE);
  CHECK(urls.size() == 1 && urls[0] == ricky_ota::COS_MANIFEST_URL);
  // Both routes down: a network error, nothing offered.
  reset();
  networkError = true;
  updater = OtaUpdater{};
  CHECK(updater.checkForUpdate(OtaUpdater::Channel::Stable) == OtaUpdater::HTTP_ERROR);
  CHECK(urls.size() == 2 && !updater.isUpdateNewer());
  std::printf(
      "RickyOS OTA: %zu checks passed (production parser/updater, real host SHA-256, simulated network/Flash).\n",
      checks);
}
