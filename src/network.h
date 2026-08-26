#pragma once

// Настройка Wi-Fi (captive portal через WiFiManager) + локальный веб-
// интерфейс (ESPAsyncWebServer, REST-эндпоинты поверх настроек в NVS).
// Работает как netTask на ядре 1, с самым низким приоритетом — без
// требований реального времени.
namespace Network {

void begin();

}  // namespace Network
