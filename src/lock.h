#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// One recursive mutex guards all controller/settings/profile state. The
// control loop runs in the Arduino loop task while web handlers run in the
// AsyncTCP task, so every access from either side takes this lock.
inline SemaphoreHandle_t stateMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
  return m;
}

class LockGuard {
 public:
  LockGuard() { xSemaphoreTakeRecursive(stateMutex(), portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGiveRecursive(stateMutex()); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};
