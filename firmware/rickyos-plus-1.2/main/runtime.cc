// Full RickyOS 1.2 application; hardware acceptance remains pending.
#include <Arduino.h>
#include <BoardConfig.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
static_assert(FREEINK_MCU_S31 && !FREEINK_MCU_S3);
static_assert(BoardConfig::MAX_FRAMEBUFFER_BYTES == 103968);
extern void setup();
extern void loop();
namespace {
void applicationTask(void*) {
  setup();
  for (;;) { loop(); vTaskDelay(pdMS_TO_TICKS(1)); }
}
}
extern "C" void app_main() {
  initArduino();
  if (xTaskCreate(applicationTask, "RickyOS", 32768, nullptr, 1, nullptr) != pdPASS)
    ESP_LOGE("RickyOS", "application task allocation failed");
}
