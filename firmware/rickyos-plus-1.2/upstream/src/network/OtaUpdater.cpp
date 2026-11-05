#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <mbedtls/sha256.h>

#include "RickyOtaManifest.h"
#include "RickyOtaNotes.h"
#include "RickyOtaTrust.h"
#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
struct ScopedSha {
  mbedtls_sha256_context context;
  ScopedSha() { mbedtls_sha256_init(&context); }
  ~ScopedSha() { mbedtls_sha256_free(&context); }
};
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(const Channel requestedChannel) {
  // Rechecking (including a failed/empty check) revokes any previous offer.
  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaRootCa = nullptr;
  otaSize = totalSize = processedSize = releaseNoteCount = 0;
  (void)requestedChannel;
  channel = Channel::Stable;
  expectedSha.fill(0);
  if (std::string_view(board_tag::boardName(), board_tag::boardNameLen()) != "readpico") return WRONG_DEVICE_ERROR;
  // The COS bucket first (mainland China), then the site; each route has its own pinned
  // root. The first route that answers decides: an authoritative "no update" or a
  // broken manifest from it is the answer, a network failure moves on.
  struct Route {
    const char* base;
    const char* manifest;
    const char* notes;
    const char* rootCa;
  };
  static constexpr Route kRoutes[] = {
      {ricky_ota::COS_BASE, ricky_ota::COS_MANIFEST_URL, ricky_ota::COS_NOTES_URL, ricky_ota::COS_ROOT_CA},
      {ricky_ota::SITE_BASE, ricky_ota::MANIFEST_URL, ricky_ota::NOTES_URL, ricky_ota::ROOT_CA},
  };
  // Fixed, bounded stack state instead of the shared 2 KiB SAX token or a full
  // JSON body allocation. It is released when this one network request ends.
  static_assert(sizeof(RickyOtaManifest) <= 512);
  const Route* route = nullptr;
  RickyOtaManifest manifest;
  for (const auto& candidate : kRoutes) {
    if (candidate.base == ricky_ota::COS_BASE && !ricky_ota::COS_ENABLED) continue;
    manifest = RickyOtaManifest{};
    if (HttpDownloader::fetchVerifiedUrl(
            candidate.manifest, [&manifest](const uint8_t* data, size_t len) { return manifest.feed(data, len); },
            candidate.rootCa)) {
      route = &candidate;
      break;
    }
    LOG_ERR("OTA", "Manifest unreachable: %s", candidate.manifest);
  }
  if (!route) return HTTP_ERROR;
  if (!manifest.valid()) return JSON_PARSE_ERROR;
  if (!manifest.hasUpdate()) return NO_UPDATE;
  latestVersion = manifest.getVersion();
  // Only a validated relative filename on an owned HTTPS route, never a URL
  // supplied by an upstream release or a locale-selected CrossMux endpoint.
  otaUrl = route->base;
  otaUrl += manifest.getFile();
  otaRootCa = route->rootCa;
  otaSize = totalSize = manifest.getBytes();
  expectedSha = manifest.getSha();
  updateAvailable = true;
  LOG_INF("OTA", "RickyOS stable offer: %s (%zu bytes) from %s", latestVersion.c_str(), otaSize, route->base);
  // What's new: optional and separate, so the offer never waits on or fails for it.
  RickyOtaNotes<ReleaseJsonParser::RELEASE_NOTE_SIZE> notes(releaseNotes, latestVersion);
  if (HttpDownloader::fetchVerifiedUrl(
          route->notes, [&notes](const uint8_t* data, size_t len) { return notes.feed(data, len); }, route->rootCa)) {
    releaseNoteCount = notes.count();
  }
  LOG_INF("OTA", "Release notes: %zu", releaseNoteCount);
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  return updateAvailable && ricky_ota::newer(CROSSPOINT_VERSION, latestVersion);
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }
  const auto* runningPartition = esp_ota_get_running_partition();
  if (!runningPartition || updatePartition->address == runningPartition->address || otaSize < 24 ||
      updatePartition->size < ricky_ota::RESERVE_BYTES ||
      ((otaSize + 4095u) & ~4095u) > updatePartition->size - ricky_ota::RESERVE_BYTES) {
    LOG_ERR("OTA", "RickyOS image does not fit a separate OTA slot with its reserve");
    return INTERNAL_UPDATE_ERROR;
  }
  ScopedSha sha;
  if (mbedtls_sha256_starts(&sha.context, 0) != 0) return INTERNAL_UPDATE_ERROR;
  ricky_ota::StringScanner brandScanner("RickyOS");
  // Match the complete, NUL-terminated firmware version, not a prefix of a
  // different revision. Both scanners use only fixed stack state.
  ricky_ota::StringScanner versionScanner(std::string_view(latestVersion.c_str(), latestVersion.size() + 1));

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition,
                                    otaSize,
                                    &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  board_tag::Scanner boardScanner;
  bool wrongBoard = false;
  const bool fetchOk = HttpDownloader::
      fetchVerifiedUrl
      (
          otaUrl,
          [&](const uint8_t* data, size_t len) {
            if (processedSize > totalSize || len > totalSize - processedSize) return false;
            if (mbedtls_sha256_update(&sha.context, data, len) != 0) {
              flashOk = false;
              return false;
            }
            brandScanner.feed(data, len);
            versionScanner.feed(data, len);
            if (hdrLen < sizeof(hdr)) {
              const size_t take = std::min(len, sizeof(hdr) - hdrLen);
              std::memcpy(hdr + hdrLen, data, take);
              hdrLen += take;
              if (hdrLen == sizeof(hdr)) {
                uint16_t imageChip;
                std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
                const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
                if (hdr[0] != 0xe9 || hdr[1] < 1 || hdr[1] > 16 || imageChip != 9 || deviceChip != 9) {
                  wrongChip = true;
                  return false;
                }
                if (deviceChip != 0xFFFF && imageChip != deviceChip) {
                  LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
                  wrongChip = true;
                  return false;  // abort the transfer
                }
              }
            }
            boardScanner.feed(data, len);
            if (boardScanner.mismatch()) {
              LOG_ERR("OTA", "wrong board: image=%s device=%.*s", boardScanner.foundName(),
                      static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
              wrongBoard = true;
              return false;  // abort before selecting the incomplete image as bootable
            }
            if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
              flashOk = false;
              return false;  // abort the transfer
            }
            processedSize += len;
            // Fire the callback only on whole-percent change. Per-chunk updates wake the
            // render task, whose framebuffer work contends with TLS on the internal arena,
            // and e-ink can't repaint faster than a percent tick anyway.
            if (onProgress && totalSize > 0) {
              const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
              if (pct != lastReportedPct) {
                lastReportedPct = pct;
                onProgress(ctx);
              }
            }
            return true;
          }
          ,
          otaRootCa ? otaRootCa : ricky_ota::ROOT_CA
      );

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongChip || wrongBoard) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }
  std::array<uint8_t, 32> actualSha{};
  if (processedSize != totalSize || mbedtls_sha256_finish(&sha.context, actualSha.data()) != 0 ||
      actualSha != expectedSha || !brandScanner.found() || !versionScanner.found() || !boardScanner.found()) {
    LOG_ERR("OTA", "RickyOS firmware size, SHA-256, brand, version or required board tag mismatch");
    esp_ota_abort(otaHandle);
    updateAvailable = false;
    return JSON_PARSE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
