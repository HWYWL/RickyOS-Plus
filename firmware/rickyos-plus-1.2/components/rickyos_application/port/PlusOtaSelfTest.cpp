// Diagnostic only (RICKYOS_PLUS_OTA_SELFTEST=1): after the saved Wi-Fi joins, run the
// production OtaUpdater end to end (check, download, verify, select) and restart into
// the update. Test builds point COS at a test folder (RICKYOS_OTA_COS_DIR) and drop the
// site route, so the official manifests are never involved.
#if RICKYOS_PLUS_OTA_SELFTEST
#include <Arduino.h>
#include <Logging.h>
#include <esp_system.h>

#include "HalPowerManager.h"
#include "network/OtaUpdater.h"
#include "network/RickyOtaPolicy.h"

void plusOtaSelfTestOnConnected() {
  HalPowerManager::Lock performance;
  OtaUpdater updater;
  const uint32_t started = millis();
  const auto checked = updater.checkForUpdate(OtaUpdater::Channel::Stable);
  LOG_INF("OTATEST", "check result=%d ms=%lu running=%s offered=%s newer=%d manifest=%s", static_cast<int>(checked),
          static_cast<unsigned long>(millis() - started), CROSSPOINT_VERSION, updater.getLatestVersion().c_str(),
          updater.isUpdateNewer(), ricky_ota::COS_MANIFEST_URL);
  if (checked != OtaUpdater::OK || !updater.isUpdateNewer()) return;
  const uint32_t install = millis();
  const auto installed = updater.installUpdate(nullptr, nullptr);
  LOG_INF("OTATEST", "install result=%d ms=%lu", static_cast<int>(installed),
          static_cast<unsigned long>(millis() - install));
  if (installed != OtaUpdater::OK) return;
  LOG_INF("OTATEST", "restarting into the update");
  delay(500);
  esp_restart();
}
#endif
