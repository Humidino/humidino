#include "watchdog.h"

#include <esp_task_wdt.h>

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

}  // namespace Watchdog
