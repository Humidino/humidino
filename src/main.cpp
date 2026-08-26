#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "display.h"
#include "network.h"
#include "relay.h"
#include "sensors.h"
#include "settings_store.h"
#include "shared_state.h"
#include "watchdog.h"

void setup() {
    Serial.begin(115200);

    Watchdog::begin();
    ShaState::begin();
    Settings::begin();

    ShaState::updateSettings(Settings::load());

    Sensors::begin();
    Relay::begin();
    Display::begin();
    Network::begin();

    Watchdog::registerCurrentTask();
}

void loop() {
    Watchdog::feed();
    vTaskDelay(pdMS_TO_TICKS(500));
}
