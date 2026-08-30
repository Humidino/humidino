#include "watchdog.h"

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

namespace Watchdog {

void begin() {
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
}

void registerCurrentTask() {
    esp_task_wdt_add(nullptr);
}

void feed() {
    esp_task_wdt_reset();
}

void unregisterCurrentTask() {
    // Явный хендл вместо nullptr — в отличие от add()/reset(), не во всех
    // версиях esp_task_wdt delete() однозначно трактует NULL как "текущая
    // задача", а xTaskGetCurrentTaskHandle() работает всегда одинаково.
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
}

}  // namespace Watchdog
