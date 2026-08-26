#include "network.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mqtt.h"
#include "shared_state.h"
#include "watchdog.h"
#include "web_server.h"
#include "wifi_provision.h"

namespace {

void netTask(void*) {
    Watchdog::registerCurrentTask();

    WifiProvision::begin();  // блокирует только эту задачу до подключения/таймаута
    LocalWebServer::begin();
    Mqtt::begin();

    for (;;) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        ShaState::updateWifi(connected, connected ? static_cast<int8_t>(WiFi.RSSI()) : 0);

        Mqtt::loop();

        Watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

}  // namespace

namespace Network {

void begin() {
    xTaskCreatePinnedToCore(netTask, "netTask", 8192, nullptr, 1, nullptr, 1);
}

}  // namespace Network
