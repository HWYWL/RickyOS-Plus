#include "plus_i2c_wire.h"
#include <Wire.h>
#include <atomic>

namespace {
std::atomic<bool> initialized{false};
esp_err_t status(uint8_t result) {
  if (result == 0) return ESP_OK;
  if (result == 2 || result == 3) return ESP_ERR_NOT_FOUND;
  return result == 5 ? ESP_ERR_TIMEOUT : ESP_FAIL;
}
}
extern "C" bool plus_i2c_initialized() { return initialized.load(); }
extern "C" esp_err_t plus_i2c_begin() {
  if (initialized.load()) return ESP_OK;
  // Wire owns one new-driver I2C bus and its transaction buffers/locks. Reuse
  // it for the vendor TPS/TCA and the SDK gauge/RTC/touch; do not install legacy.
  if (!Wire1.begin(0, 1, 100000)) return ESP_FAIL;
  Wire1.setTimeOut(100);
  initialized.store(true);
  return ESP_OK;
}
extern "C" esp_err_t plus_i2c_probe(uint8_t address) {
  if (!initialized.load()) return ESP_ERR_INVALID_STATE;
  Wire1.beginTransmission(address);
  return status(Wire1.endTransmission(true));
}
extern "C" esp_err_t plus_i2c_read(uint8_t address, uint8_t reg, uint8_t* data, size_t size) {
  if (!initialized.load()) return ESP_ERR_INVALID_STATE;
  if (size == 0) return ESP_OK;
  if (!data || size > 128) return ESP_ERR_INVALID_ARG;
  Wire1.beginTransmission(address);
  Wire1.write(reg);
  // Wire retains its own mutex from this repeated start through requestFrom.
  esp_err_t result = status(Wire1.endTransmission(false));
  if (result != ESP_OK) return result;
  if (Wire1.requestFrom(address, size, true) != size) {
    while (Wire1.available()) (void)Wire1.read();
    return ESP_FAIL;
  }
  for (size_t i = 0; i < size; ++i) data[i] = Wire1.read();
  return ESP_OK;
}
extern "C" esp_err_t plus_i2c_write(uint8_t address, uint8_t reg, const uint8_t* data, size_t size) {
  if (!initialized.load()) return ESP_ERR_INVALID_STATE;
  if ((size && !data) || size > 127) return ESP_ERR_INVALID_ARG;
  Wire1.beginTransmission(address);
  Wire1.write(reg);
  for (size_t i = 0; i < size; ++i) Wire1.write(data[i]);
  return status(Wire1.endTransmission(true));
}
