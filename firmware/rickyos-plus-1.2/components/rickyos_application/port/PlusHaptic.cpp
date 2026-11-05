#include "PlusHaptic.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
extern "C" {
#include "pca9555.h"
}
namespace plus_haptic {
namespace {
constexpr int64_t kPulseUs = 35000;
constexpr uint64_t kRetryUs = 20000;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
esp_timer_handle_t timer = nullptr;
int64_t deadline = 0;
bool stopping = false;

// The existing ESP timer task owns expiry: no UI-loop timing or dedicated task.
// Every OFF failure is retried; new pulses cannot extend an active request.
void stopLocked() {
  deadline = 0;
  if (timer) esp_timer_stop(timer);
  stopping = pca9555_set_motor(false) != ESP_OK;
  if (stopping && timer) esp_timer_start_once(timer, kRetryUs);
}
void expired(void*) {
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  if (stopping || (deadline && esp_timer_get_time() >= deadline)) stopLocked();
  xSemaphoreGive(mutex);
}
}
bool begin() {
  if (timer) return true;
  if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (!mutex) return false;
  esp_timer_create_args_t args{};
  args.callback = expired;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "plus_haptic";
  // One lifetime esp_timer allocation; IDF has no static timer allocation API.
  // Mutex/state are static; no per-click buffers, allocations or new task.
  if (esp_timer_create(&args, &timer) != ESP_OK) return false;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    stopLocked();
    xSemaphoreGive(mutex);
  }
  ESP_LOGI("PlusHaptic", "TCA P0.3 ready; pulse=35ms");
  return true;
}
void stop() {
  if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  if (deadline || stopping) stopLocked();
  xSemaphoreGive(mutex);
}
void pulse() {
  if (!timer || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  if (!deadline && !stopping) {
    deadline = esp_timer_get_time() + kPulseUs;
    if (pca9555_set_motor(true) != ESP_OK || esp_timer_start_once(timer, kPulseUs) != ESP_OK) stopLocked();
  }
  xSemaphoreGive(mutex);
}
}
