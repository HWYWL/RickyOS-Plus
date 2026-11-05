#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool plus_i2c_initialized(void);
esp_err_t plus_i2c_begin(void);
esp_err_t plus_i2c_probe(uint8_t address);
esp_err_t plus_i2c_read(uint8_t address, uint8_t reg, uint8_t* data, size_t size);
esp_err_t plus_i2c_write(uint8_t address, uint8_t reg, const uint8_t* data, size_t size);
#ifdef __cplusplus
}
#endif
