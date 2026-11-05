#pragma once
#include <cstddef>
#include <cstdint>
struct esp_partition_t {
  uint32_t address;
  uint32_t size;
};
using esp_ota_handle_t = unsigned;
using esp_err_t = int;
inline constexpr int ESP_OK = 0;
inline constexpr size_t OTA_SIZE_UNKNOWN = static_cast<size_t>(-1);
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
inline const char* esp_err_to_name(esp_err_t) { return "host stub"; }
